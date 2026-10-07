// Recovered from repository commits 581dad7d and 5b826c6e. Their runner entries
// survived while the case definitions were absent from the current source.
TestCase VectorCompareF64Edges() {
  using O = ShaderOpcode;
  constexpr std::array<std::array<uint64_t, 2>, 12> pairs{{
      {0, 0x8000000000000000ull}, {1, 0}, {1, 1}, {1, 2},
      {0x11d04c7b00ull, 0}, {0x3ff0000000000000ull, 0x3ff0000000000001ull},
      {0xbff0000000000000ull, 0xbff0000000000000ull},
      {0x7ff0000000000000ull, 0x7ff0000000000000ull},
      {0x7ff0000000000000ull, 0xfff0000000000000ull},
      {0x7ff8000000000001ull, 0x7ff8000000000001ull},
      {0x7ff0000000000001ull, 0}, {0x7fefffffffffffffull, 0x7fefffffffffffffull}}};
  TestCase test;
  test.name = "VectorCompareF64Edges";
  for (const auto& pair : pairs)
    for (const auto bits : pair)
      test.initial.insert(test.initial.end(), {u32(bits), u32(bits >> 32)});
  test.expected = test.initial;
  const auto store_mask = [&](u32 sgpr, bool value) {
    AppendStoreSgprPair(&test.code, sgpr, static_cast<u32>(test.expected.size()));
    test.expected.insert(test.expected.end(), {u32(value), 0});
  };
  for (u32 i = 0; i < pairs.size(); ++i) {
    for (u32 word = 0; word < 4; ++word) {
      AppendVMovU32(&test.code, 31, (i * 4 + word) * 4);
      AppendBufferLoadDword(&test.code, 1 + word, 31);
    }
    const auto lhs = std::bit_cast<double>(pairs[i][0]);
    const auto rhs = std::bit_cast<double>(pairs[i][1]);
    test.code.push_back(EncodeVopc(0x22, Vgpr(1), 3));
    store_mask(106, lhs == rhs);
    AppendVop3(&test.code, 0x22, 20, Vgpr(1), Vgpr(3), 0, 1, 0, false, 0, 1);
    store_mask(20, -std::abs(lhs) == rhs);
    test.code.push_back(EncodeVop1(0x02, 100, Vgpr(3)));
    test.code.push_back(EncodeVop1(0x02, 101, Vgpr(4)));
    // Captured e6d76d24f59f8015 PC 0xe4: compare zero with s100:s101.
    test.code.insert(test.code.end(), {0xd422006au, 0x0000c880u});
    store_mask(106, 0.0 == rhs);
  }
  AppendEnd(&test.code);
  test.initial.resize(test.expected.size());
  test.opcodes = {O::V_MOV_B32, O::BUFFER_LOAD_DWORD, O::V_READFIRSTLANE_B32,
                  O::V_CMP_EQ_F64, O::BUFFER_STORE_DWORD, O::S_ENDPGM};
  test.required_spirv = {"OpCapability Float64", "OpTypeFloat 64", "OpFOrdEqual"};
  return test;
}


TestCase VectorCompareF64WaveMasks(u32 wave_size) {
  using O = ShaderOpcode;
  constexpr std::array<uint64_t, 4> values{0, 1, 0x8000000000000000ull,
                                          0x7ff8000000000000ull};
  constexpr u32 low_exec = 0xeeeeeeeeu, high_exec = 0x77777777u;
  constexpr u32 vcc_hi = 0x89abcdefu;
  TestCase test;
  test.name = wave_size == 32 ? "VectorCompareF64Wave32Masks" : "VectorCompareF64Wave64Masks";
  test.initial.assign(6 * wave_size, 0);
  for (u32 lane = 0; lane < wave_size; ++lane) {
    test.initial[lane] = u32(values[lane % 4]);
    test.initial[wave_size + lane] = u32(values[lane % 4] >> 32);
  }
  test.expected = test.initial;
  for (u32 lane = 0; lane < wave_size; ++lane) {
    test.expected[2 * wave_size + lane] = low_exec & 0x55555555u;
    test.expected[3 * wave_size + lane] = wave_size == 64 ? high_exec & 0x55555555u : vcc_hi;
    test.expected[4 * wave_size + lane] = low_exec;
    test.expected[5 * wave_size + lane] = wave_size == 64 ? high_exec : 0;
  }
  auto& code = test.code;
  code.push_back(EncodeVop2(0x1a, 30, InlineU32(2), 0));
  AppendBufferLoadDword(&code, 1, 30);
  AppendVMovU32(&code, 31, wave_size * 4);
  code.push_back(EncodeVop2(0x25, 31, Vgpr(31), 30));
  AppendBufferLoadDword(&code, 2, 31);
  code.push_back(EncodeSop1(0x04, 10, 126));
  AppendSMovLiteral(&code, 107, vcc_hi);
  AppendSMovLiteral(&code, 126, low_exec);
  if (wave_size == 64) AppendSMovLiteral(&code, 127, high_exec);
  AppendVop3(&code, 0x22, 106, InlineU32(0), Vgpr(1));
  code.push_back(EncodeSMovB32(20, 126));
  code.push_back(EncodeSMovB32(21, 127));
  code.push_back(EncodeSop1(0x04, 126, 10));
  for (const auto [reg, offset] : std::array<std::pair<u32, u32>, 4>{{
           {106, 2}, {107, 3}, {20, 4}, {21, 5}}})
    AppendStoreSgprAtLaneDwordOffset(&code, reg, 0, offset * wave_size);
  AppendEnd(&code);
  test.compute_info.threads_num[0] = wave_size;
  test.compute_info.threads_num[1] = test.compute_info.threads_num[2] = 1;
  test.compute_info.thread_ids_num = 1;
  test.compute_info.wave_size = wave_size;
  test.has_compute_info = true;
  test.opcodes = {O::V_MOV_B32, O::S_MOV_B32, O::S_MOV_B64, O::V_LSHLREV_B32,
                  O::V_ADD_NC_U32, O::BUFFER_LOAD_DWORD, O::V_CMP_EQ_F64,
                  O::BUFFER_STORE_DWORD, O::S_ENDPGM};
  test.required_spirv = {"OpFOrdEqual"};
  return test;
}


TestCase VectorCompareInteger64Edges() {
  using O = ShaderOpcode;
  constexpr std::array<std::array<uint64_t, 2>, 10> pairs{{
      {0, 0}, {0, 1}, {1, 0}, {0xffffffffull, 0x100000000ull},
      {0x100000000ull, 0xffffffffull}, {0x8000000000000000ull, 0},
      {0x7fffffffffffffffull, 0x8000000000000000ull},
      {0xffffffffffffffffull, 0xffffffffffffffffull},
      {0xffffffff80000000ull, 0xffffffff7fffffffull},
      {0x1234567880000000ull, 0x123456787fffffffull}}};
  TestCase test;
  test.name = "VectorCompareInteger64Edges";
  for (const auto& pair : pairs)
    for (const auto bits : pair)
      test.initial.insert(test.initial.end(), {u32(bits), u32(bits >> 32)});
  test.expected = test.initial;
  const auto store_mask = [&](u32 sgpr, bool value) {
    AppendStoreSgprPair(&test.code, sgpr, static_cast<u32>(test.expected.size()));
    test.expected.insert(test.expected.end(), {u32(value), 0});
  };
  for (u32 i = 0; i < pairs.size(); ++i) {
    for (u32 word = 0; word < 4; ++word) {
      AppendVMovU32(&test.code, 31, (i * 4 + word) * 4);
      AppendBufferLoadDword(&test.code, 1 + word, 31);
    }
    test.code.push_back(EncodeVop1(0x02, 8, Vgpr(3)));
    test.code.push_back(EncodeVop1(0x02, 9, Vgpr(4)));
    const auto lhs = pairs[i][0], rhs = pairs[i][1];
    const auto signed_lhs = std::bit_cast<int64_t>(lhs);
    const auto signed_rhs = std::bit_cast<int64_t>(rhs);
    for (const auto [opcode, value] : std::array<std::pair<u32, bool>, 5>{{
             {0xa1, signed_lhs < signed_rhs}, {0xa3, signed_lhs <= signed_rhs},
             {0xa5, lhs != rhs},
             {0xe3, lhs <= rhs}, {0xe6, lhs >= rhs}}}) {
      test.code.push_back(EncodeVopc(opcode, Vgpr(1), 3));
      store_mask(106, value);
      AppendVop3(&test.code, opcode, 20, Vgpr(1), 8);
      store_mask(20, value);
    }
    // A negative literal expands by signedness, including equality comparisons.
    for (const auto [opcode, value] : std::array<std::pair<u32, bool>, 5>{{
             {0xa1, -1 < signed_rhs}, {0xa3, -1 <= signed_rhs},
             {0xa5, -1 != signed_rhs},
             {0xa2, -1 == signed_rhs}, {0xe3, 0xffffffffull <= rhs}}}) {
      test.code.push_back(EncodeVopc(opcode, 255, 3));
      test.code.push_back(0xffffffffu);
      store_mask(106, value);
    }
    AppendVop3(&test.code, 0xa3, 20, Vgpr(1), 255);
    test.code.push_back(0xffffffffu);
    store_mask(20, signed_lhs <= -1);
    // Captured 8395e43f382309df PC 0x20cc: s[2:3] = (s[0:1] != 0) & EXEC.
    // Save the test buffer descriptor while its SGPRs hold the captured operands.
    test.code.push_back(EncodeSop1(0x04, 32, 0));
    test.code.push_back(EncodeSop1(0x04, 34, 2));
    test.code.push_back(EncodeVop1(0x02, 0, Vgpr(1)));
    test.code.push_back(EncodeVop1(0x02, 1, Vgpr(2)));
    test.code.insert(test.code.end(), {0xd4a50002u, 0x00010000u});
    test.code.push_back(EncodeSop1(0x04, 20, 2));
    test.code.push_back(EncodeSop1(0x04, 0, 32));
    test.code.push_back(EncodeSop1(0x04, 2, 34));
    store_mask(20, lhs != 0);
  }
  AppendEnd(&test.code);
  test.initial.resize(test.expected.size());
  test.opcodes = {O::V_MOV_B32, O::BUFFER_LOAD_DWORD, O::V_READFIRSTLANE_B32,
                  O::V_CMP_LT_I64, O::V_CMP_LE_I64, O::V_CMP_EQ_I64, O::V_CMP_NE_I64,
                  O::V_CMP_LE_U64, O::V_CMP_GE_U64,
                  O::S_MOV_B64, O::BUFFER_STORE_DWORD, O::S_ENDPGM};
  test.required_spirv = {"OpSLessThan", "OpULessThanEqual", "OpUGreaterThanEqual"};
  return test;
}


TestCase VectorCompareExecWaveMasks(u32 wave_size) {
  using O = ShaderOpcode;
  // Rows exercise signed 16-bit truncation, full-width unsigned ordering, and
  // ordered FP64 comparisons (including NaN, signed zero, and subnormals).
  constexpr std::array<std::array<std::array<uint64_t, 2>, 8>, 3> inputs{{
      {{{0x1111ffff, 0x22220000}, {0x33338000, 0x44447fff},
        {0x55550000, 0x66660000}, {0x77777fff, 0x88888000},
        {0x99990001, 0xaaaa0002}, {0xbbbb0002, 0xcccc0001},
        {0xddddffff, 0xeeeeffff}, {0xffff8001, 0x11118000}}},
      {{{0, 0}, {0, 1}, {1, 0}, {0xffffffffull, 0x100000000ull},
        {0x100000000ull, 0xffffffffull}, {0x8000000000000000ull, 0},
        {0xffffffffffffffffull, 0xffffffffffffffffull},
        {0x1234567880000000ull, 0x123456787fffffffull}}},
      {{{0, 0x8000000000000000ull}, {1, 0}, {0x8000000000000001ull, 0},
        {0x7ff8000000000001ull, 0}, {0, 0x7ff0000000000001ull},
        {0xc000000000000000ull, 0xbff0000000000000ull},
        {0x7ff0000000000000ull, 0x7ff0000000000000ull},
        {0x3ff0000000000001ull, 0x3ff0000000000000ull}}}}};
  struct Compare { u32 encoding; O opcode; u32 group; };
  constexpr std::array comparisons{
      Compare{0x99, O::V_CMPX_LT_I16, 0}, Compare{0x9a, O::V_CMPX_EQ_I16, 0},
      Compare{0x9b, O::V_CMPX_LE_I16, 0}, Compare{0x9c, O::V_CMPX_GT_I16, 0},
      Compare{0x9d, O::V_CMPX_NE_I16, 0}, Compare{0x9e, O::V_CMPX_GE_I16, 0},
      Compare{0xf3, O::V_CMPX_LE_U64, 1}, Compare{0x33, O::V_CMPX_LE_F64, 2},
      Compare{0x36, O::V_CMPX_GE_F64, 2}};
  constexpr u32 low_exec = 0xeeeeeeeeu, high_exec = 0x77777777u;
  constexpr u32 vcc_lo = 0x12345678u, vcc_hi = 0x89abcdefu;
  TestCase test;
  test.name = wave_size == 32 ? "VectorCompareExecWave32Masks" : "VectorCompareExecWave64Masks";
  test.initial.assign(12 * wave_size, 0);
  for (u32 group = 0; group < inputs.size(); ++group)
    for (u32 lane = 0; lane < wave_size; ++lane)
      for (u32 word = 0; word < 4; ++word)
        test.initial[(group * 4 + word) * wave_size + lane] =
            u32(inputs[group][lane % 8][word / 2] >> (32 * (word % 2)));
  test.expected = test.initial;
  auto& code = test.code;
  code.push_back(EncodeSop1(0x04, 10, 126));
  AppendSMovLiteral(&code, 106, vcc_lo);
  AppendSMovLiteral(&code, 107, vcc_hi);
  for (const auto compare : comparisons) {
    for (u32 word = 0; word < 4; ++word) {
      code.push_back(EncodeVop2(0x1a, 30, InlineU32(2), 0));
      AppendVMovU32(&code, 31, (compare.group * 4 + word) * wave_size * 4);
      code.push_back(EncodeVop2(0x25, 31, Vgpr(31), 30));
      AppendBufferLoadDword(&code, 1 + word, 31);
    }
    std::array<u32, 2> expected_exec{};
    for (u32 lane = 0; lane < wave_size; ++lane) {
      const auto pair = inputs[compare.group][lane % 8];
      const auto a = std::bit_cast<int16_t>(uint16_t(pair[0]));
      const auto b = std::bit_cast<int16_t>(uint16_t(pair[1]));
      bool result = false;
      switch (compare.encoding) {
        case 0x99: result = a < b; break;
        case 0x9a: result = a == b; break;
        case 0x9b: result = a <= b; break;
        case 0x9c: result = a > b; break;
        case 0x9d: result = a != b; break;
        case 0x9e: result = a >= b; break;
        case 0xf3: result = pair[0] <= pair[1]; break;
        case 0x33: result = std::bit_cast<double>(pair[0]) <= std::bit_cast<double>(pair[1]); break;
        case 0x36: result = std::bit_cast<double>(pair[0]) >= std::bit_cast<double>(pair[1]); break;
      }
      if (result) expected_exec[lane / 32] |= 1u << (lane % 32);
    }
    expected_exec[0] &= low_exec;
    expected_exec[1] &= high_exec;
    for (const bool vop3 : {false, true}) {
      AppendSMovLiteral(&code, 126, low_exec);
      if (wave_size == 64) AppendSMovLiteral(&code, 127, high_exec);
      if (vop3) AppendVop3(&code, compare.encoding, 20, Vgpr(1), Vgpr(3));
      else code.push_back(EncodeVopc(compare.encoding, Vgpr(1), 3));
      code.push_back(EncodeSMovB32(20, 126));
      code.push_back(EncodeSMovB32(21, 127));
      code.push_back(EncodeSop1(0x04, 126, 10));
      for (const auto [reg, value] : std::array<std::pair<u32, u32>, 4>{{
               {20, expected_exec[0]}, {21, expected_exec[1]},
               {106, vcc_lo}, {107, vcc_hi}}}) {
        AppendStoreSgprAtLaneDwordOffset(&code, reg, 0, u32(test.expected.size()));
        test.expected.insert(test.expected.end(), wave_size, value);
      }
    }
    test.opcodes.push_back(compare.opcode);
  }
  AppendEnd(&code);
  test.initial.resize(test.expected.size());
  test.compute_info.threads_num[0] = wave_size;
  test.compute_info.threads_num[1] = test.compute_info.threads_num[2] = 1;
  test.compute_info.thread_ids_num = 1;
  test.compute_info.wave_size = wave_size;
  test.has_compute_info = true;
  test.opcodes.insert(test.opcodes.end(), {O::V_MOV_B32, O::S_MOV_B32, O::S_MOV_B64,
      O::V_LSHLREV_B32, O::V_ADD_NC_U32, O::BUFFER_LOAD_DWORD,
      O::BUFFER_STORE_DWORD, O::S_ENDPGM});
  test.required_spirv = {"OpFOrdLessThanEqual", "OpFOrdGreaterThanEqual"};
  return test;
}
