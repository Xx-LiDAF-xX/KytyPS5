// Adapted from Jetsku/KytyPS5 ShaderSrtVariantTests.inc at 58a7f743.
// Keep this branch's independently aligned scalar offsets and exercise real GPU reads.
TestCase ScalarBufferLoadsGpuSelectedDescriptors() {
  using O = ShaderOpcode;
  constexpr uint64_t GuestBase = 0x0000000118000000ull;
  struct DescriptorCase {
    u32 stride, records, offset, soffset;
  };
  const DescriptorCase cases[] = {
      {0, 16, 8, 0},  // dwords 2-3 in, 4-5 past the 16-byte bound
      {0, 24, 8, 0},  // all four in
      {12, 2, 12, 0}, // stride * records = 24 bytes: dwords 3-5 in, 6 out
      {0, 0, 0, 0},   // empty
      {0, 20, 4, 8},  // SOFFSET is added: dwords 3-4 in, 5-6 out
      {0, 32, 10, 0}, // the two LSBs of the offset are ignored: reads from byte 8
      {4, 5, 8, 0},   // 20 bytes: dwords 2-4 in, 5 out
      {0, 15, 8, 0},  // 15 bytes: dword 2 in, dword 3 (bytes 12-15) out
      {0, 32, 3, 3},  // align immediate and SOFFSET independently
  };
  constexpr u32 Count = static_cast<u32>(std::size(cases));
  TestCase test;
  test.name = "ScalarBufferLoadsGpuSelectedDescriptors";
  test.initial.resize(2048);
  for (u32 i = 0; i < Count; ++i) {
    const u32 data_offset = 4096 + i * 128;
    const uint64_t base = GuestBase + data_offset;
    const std::array<u32, 4> descriptor{static_cast<u32>(base),
                                        (cases[i].stride << 16u) | static_cast<u32>(base >> 32u),
                                        cases[i].records, 0x5204u};
    std::copy(descriptor.begin(), descriptor.end(), test.initial.begin() + 130 + i * 30);
    for (u32 word = 0; word < 32; ++word) {
      test.initial[data_offset / 4 + word] = i * 100 + word + 1;
    }
  }
  for (u32 i = 0; i < Count; ++i) {
    // Select the table entry through a GPU load and readfirstlane, not host constants.
    const u32 selected = Count - 1 - i;
    const auto &input = cases[selected];
    test.initial[64 + i] = selected;
    AppendVMovU32(&test.code, 30, (64 + i) * 4);
    AppendBufferLoadDword(&test.code, 0, 30);
    test.code.push_back(EncodeVop1(0x02, 20, Vgpr(0))); // v_readfirstlane_b32 s20, v0
    test.code.push_back(EncodeSop2(0x26, 20, 20, 255)); // s_mul_i32 s20, s20, 120
    test.code.push_back(120);
    test.code.push_back(EncodeSmem0(0x0a, 8, 0)); // s_buffer_load_dwordx4 s[8:11], s[0:3]
    test.code.push_back(EncodeSmem1(520, 20));
    AppendSMovLiteral(&test.code, 22, input.soffset);
    test.code.push_back(EncodeSmem0(0x0a, 12, 4)); // s_buffer_load_dwordx4 s[12:15], s[8:11]
    test.code.push_back(EncodeSmem1(input.offset, 22));
    const u32 size = input.stride == 0 ? input.records : input.stride * input.records;
    const u32 first = (input.offset & ~3u) + (input.soffset & ~3u);
    for (u32 component = 0; component < 4; ++component) {
      AppendStoreSgpr(&test.code, 12 + component, i * 4 + component);
      const u32 byte = first + component * 4;
      test.expected.push_back(byte + 4 <= size ? selected * 100 + byte / 4 + 1 : 0);
    }
  }
  AppendEnd(&test.code);
  test.bda_mappings = {{GuestBase, 0}};
  test.opcodes = {O::V_MOV_B32, O::S_MOV_B32, O::BUFFER_LOAD_DWORD, O::V_READFIRSTLANE_B32,
                  O::S_MUL_I32, O::S_BUFFER_LOAD_DWORDX4, O::BUFFER_STORE_DWORD, O::S_ENDPGM};
  test.required_spirv = {"OpConvertUToPtr", "PhysicalStorageBuffer"};
  return test;
}
