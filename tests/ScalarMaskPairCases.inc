TestCase OddScalarMaskPair(u32 wave_size) {
  using O = ShaderOpcode;
  TestCase test;
  test.name = wave_size == 32 ? "OddScalarMaskWave32" : "OddScalarMaskWave64";
  test.initial.assign(4 * wave_size, 0);
  test.expected.resize(test.initial.size());
  auto& code = test.code;
  AppendSMovLiteral(&code, 18, 0x11223344u);
  AppendSMovLiteral(&code, 19, 0x55667788u);
  AppendSMovLiteral(&code, 20, 0xdeadbeefu);
  // In wave64, encoded s19 selects s[18:19]; in wave32 it selects s19.
  code.push_back(EncodeVop3Word0(0xc2, 19));
  code.push_back(EncodeVop3Word1(InlineU32(0), Vgpr(0)));
  code.push_back(EncodeVop3Word0(0x101, 1));
  code.push_back(EncodeVop3Word1(InlineU32(23), InlineU32(17), 19));
  for (u32 component = 0; component < 3; ++component)
    AppendStoreSgprAtLaneDwordOffset(&code, 18 + component, 0, component * wave_size);
  AppendStoreVgprAtLaneDwordOffset(&code, 1, 0, 3 * wave_size);
  AppendEnd(&code);
  for (u32 lane = 0; lane < wave_size; ++lane) {
    test.expected[lane] = wave_size == 32 ? 0x11223344u : 1u;
    test.expected[wave_size + lane] = wave_size == 32 ? 1u : 0u;
    test.expected[2 * wave_size + lane] = 0xdeadbeefu;
    test.expected[3 * wave_size + lane] = lane == 0 ? 17u : 23u;
  }
  test.compute_info.threads_num[0] = wave_size;
  test.compute_info.threads_num[1] = test.compute_info.threads_num[2] = 1;
  test.compute_info.thread_ids_num = 1;
  test.compute_info.wave_size = wave_size;
  test.has_compute_info = true;
  test.opcodes = {O::S_MOV_B32, O::V_CMP_EQ_U32, O::V_CNDMASK_B32, O::V_MOV_B32,
                  O::V_ADD_NC_U32, O::V_LSHLREV_B32, O::BUFFER_STORE_DWORD, O::S_ENDPGM};
  return test;
}
