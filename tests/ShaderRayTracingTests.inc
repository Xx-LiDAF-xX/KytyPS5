void TestRayTracingInstructions() {
  namespace D = ShaderRecompiler::Decoder;
  const std::array code{
      0xf1989f01u, 0x00081014u,
      EncodeMubuf0(0x1e, 0, false), EncodeMubuf1(16, 0, 0),
      0xf1989f07u, 0x00071001u, 0x3d3e3c15u, 0x38095851u, 0x00003a39u,
      EncodeMubuf0(0x1e, 16, false), EncodeMubuf1(16, 0, 0),
      EncodeSopp(0x01)};
  D::Program decoded;
  D::DecodeProgram(code, decoded);
  Check(decoded.has_bvh && decoded.instructions.size() == 5 &&
            decoded.instructions.back().opcode == D::Opcode::S_ENDPGM,
        "decoder stopped before instructions following BVH intersections");
  for (const auto index : {0u, 2u}) {
    const auto &inst = decoded.instructions[index];
    Check(inst.opcode == D::Opcode::IMAGE_BVH_INTERSECT_RAY &&
              inst.image_address_components == 11 && inst.data_dwords == 4 && inst.image_r128,
          "BVH32 instruction lost its ray or descriptor width");
  }
  Check(decoded.instructions[2].image_nsa_addr[9] == 58,
        "BVH NSA ray components were not decoded");
  const auto result = RecompileForTest(code, MakeCompileOptions(ShaderType::Compute));
  Check(CountSourceOccurrences(result.ir_dump, "BvhIntersect") == 2 &&
            result.program.info.uses_dma && result.program.info.images.empty() &&
            result.program.info.samplers.empty(),
        "BVH intersections did not use GPU descriptors and raw guest memory");
  CheckSpirvBinaryValidates(result.spirv);

  auto disabled_options = MakeCompileOptions(ShaderType::Compute);
  disabled_options.ray_tracing_enabled = false;
  const auto disabled = ShaderRecompiler::TranslateProgram(code, disabled_options);
  Check(disabled.skip_dispatch && disabled.has_bvh,
        "disabled RT compilation did not identify and skip the BVH dispatch");
  const std::array front_bvh{0xf1989f01u, 0x00081014u, EncodeSop1(0x20, 0, 6)};
  Check(D::DecodeFrontProgram(front_bvh).has_bvh,
        "front shader decode did not preserve BVH metadata");
  const std::array plain_front{EncodeSop1(0x20, 0, 6)};
  const std::array plain_back{EncodeSopp(0x01)};
  disabled_options.back_code = plain_back;
  Check(ShaderRecompiler::TranslateProgram(front_bvh, disabled_options).skip_dispatch,
        "fused translation lost front-stage BVH metadata");
  disabled_options.back_code = code;
  Check(ShaderRecompiler::TranslateProgram(plain_front, disabled_options).skip_dispatch,
        "fused translation lost back-stage BVH metadata");
  const bool previous_rt = Config::RayTracingEnabled();
  Config::SetRayTracingEnabled(false);
  auto enabled_options = MakeCompileOptions(ShaderType::Compute);
  enabled_options.ray_tracing_enabled = true;
  const auto enabled = ShaderRecompiler::TranslateProgram(code, enabled_options);
  Config::SetRayTracingEnabled(previous_rt);
  Check(!enabled.skip_dispatch && enabled.has_bvh,
        "translation used mutable global RT policy instead of its captured options");

  const std::array literal{EncodeSMovB32(0, 255), 0xf1989f01u, EncodeSopp(0x01)};
  D::DecodeProgram(literal, decoded);
  Check(!decoded.has_bvh && decoded.instructions.size() == 2 && decoded.instructions[0].opcode == D::Opcode::S_MOV_B32,
        "BVH-shaped scalar literal was decoded as an instruction");
#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
  for (const auto words : {std::array{0xf19c9f01u, 0x00081014u}, // BVH64
                           std::array{0xf1989f01u, 0x40081014u}, // A16
                           std::array{0xf1981f01u, 0x00081014u}, // not R128
                           std::array{0xf1989f01u, 0x80081014u}}) { // D16
    const std::array unsupported{words[0], words[1], EncodeSopp(0x01)};
    ExpectFatal([&]() { (void)RecompileForTest(unsupported, MakeCompileOptions(ShaderType::Compute)); },
                "unsupported BVH encoding was silently accepted");
  }
  for (const auto nsa : {1u, 2u}) {
    std::array<uint32_t, 5> truncated{0xf1989f01u | (nsa << 1), 0x00081014u};
    truncated[2 + nsa] = EncodeSopp(0x01);
    ExpectFatal([&]() {
      (void)RecompileForTest(std::span(truncated).first(3 + nsa), MakeCompileOptions(ShaderType::Compute));
    }, "BVH NSA instruction accepted fewer than eleven ray addresses");
  }
#endif
}
