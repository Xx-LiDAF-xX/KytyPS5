// Offline diagnostic: replay a captured shader's resource refresh using only the reads logged
// after its "Shader resource input" line. Missing data fails; no guest pointer is dereferenced.
int ReplayCapturedSrt(const char* shader_path, const char* trace_path) {
  using namespace ShaderRecompiler::IR;
  std::ifstream shader(shader_path, std::ios::binary | std::ios::ate);
  Check(shader.good(), "cannot open captured shader");
  const auto bytes = shader.tellg();
  Check(bytes > 0 && bytes <= 16 * 1024 * 1024 && bytes % 4 == 0,
        "captured shader has invalid size");
  std::vector<uint32_t> code(static_cast<size_t>(bytes) / 4);
  shader.seekg(0);
  Check(static_cast<bool>(shader.read(reinterpret_cast<char*>(code.data()), bytes)),
        "cannot read captured shader");

  struct Reads {
    std::unordered_map<uint64_t, uint32_t> ordinary;
    std::unordered_map<uint64_t, uint32_t> clean;
    uint32_t missing = 0;
    uint32_t low = 0;
    uint32_t conflicts = 0;
  } reads;
  std::vector<uint32_t> user_data;
  uint64_t hash = 0;
  bool compute_input = false;
  std::ifstream trace(trace_path);
  Check(trace.good(), "cannot open captured SRT trace");
  for (std::string line; std::getline(trace, line);) {
    if (line.starts_with("Shader resource input:")) {
      compute_input = line.starts_with("Shader resource input: stage=cs hash=");
      const auto marker = line.find(" userdata:");
      Check(marker != std::string::npos, "malformed captured user data");
      const auto hash_start = line.find("hash=");
      Check(hash_start != std::string::npos, "captured input has no shader identity");
      std::istringstream identity(line.substr(hash_start + 5));
      Check(static_cast<bool>(identity >> std::hex >> hash), "malformed captured shader hash");
      std::istringstream words(line.substr(marker + 10));
      user_data.clear();
      for (uint32_t word; words >> std::hex >> word;) user_data.push_back(word);
      reads.ordinary.clear();
      reads.clean.clear();
      reads.conflicts = 0;
    } else if (!user_data.empty() && line.starts_with("SRT ") &&
               line.find(" result address=") != std::string::npos) {
      const bool clean = line.starts_with("SRT clean ");
      const bool ordinary = line.starts_with("SRT ordinary ");
      if (!clean && !ordinary) continue;
      const auto address_start = line.find("address=") + 8;
      const auto count_marker = line.find("words=");
      Check(count_marker != std::string::npos, "captured read has no word count");
      const auto count_start = count_marker + 6;
      uint64_t address = 0;
      size_t count = 0;
      std::istringstream address_text(line.substr(address_start));
      Check(static_cast<bool>(address_text >> std::hex >> address), "malformed captured address");
      std::istringstream words(line.substr(count_start));
      Check(static_cast<bool>(words >> count), "malformed captured word count");
      Check(count <= 4096, "captured read too large");
      auto& memory = clean ? reads.clean : reads.ordinary;
      for (size_t index = 0; index < count; ++index) {
        uint32_t word = 0;
        Check(static_cast<bool>(words >> std::hex >> word), "truncated captured read");
        Check(address <= UINT64_MAX - index * 4, "captured address overflows");
        const auto [entry, inserted] = memory.emplace(address + index * 4, word);
        if (!inserted && entry->second != word) ++reads.conflicts;
      }
    }
  }
  Check(compute_input && !user_data.empty() && user_data.size() <= 64,
        "trace's final shader input is not a usable compute shader input");
  if (reads.conflicts != 0) {
    std::printf("Capture is not a stable memory snapshot: %u conflicting dwords.\n",
                reads.conflicts);
    return 1;
  }
  auto options = MakeCompileOptions(ShaderType::Compute);
  options.user_data = user_data;
  options.shader_hash = hash;
  auto translated = ShaderRecompiler::TranslateProgram(code, options);
  std::printf("%s", ProgramToString(translated.program).c_str());
  const auto plan = ExtractResourcePlan(translated.program);
  std::printf("Replay hash=%016llx slots=%zu sources=%zu blocks=%zu\n",
              static_cast<unsigned long long>(hash), plan.srt_reads.size(),
              plan.descriptor_sources.size(), plan.control_flow.size());
  for (const auto& slot : plan.srt_reads) {
    const auto* inst = slot.value.Resolve().TryInstruction();
    if (inst == nullptr) continue;
    const auto flags = inst->Flags<MemoryFlags>();
    std::printf("slot %u opcode=%u pc=%x offset=%u\n", slot.flat_offset,
                static_cast<unsigned>(inst->GetOpcode()), flags.pc,
                plan.memory_info.at(flags.index).offset);
  }
  for (size_t block = 0; block < plan.control_flow.size(); ++block) {
    std::printf("resource block %zu slots:", block);
    for (const auto slot : plan.control_flow[block].srt_reads) std::printf(" %u", slot);
    std::printf("\n");
  }
  const auto read = [](Reads& capture, bool clean, uint64_t address,
                       std::span<uint32_t> values) {
    const auto& memory = clean ? capture.clean : capture.ordinary;
    for (size_t index = 0; index < values.size(); ++index) {
      if (address > UINT64_MAX - index * 4) return false;
      const auto found = memory.find(address + index * 4);
      if (found == memory.end()) {
        ++capture.missing;
        capture.low += address < 0x10000;
        std::printf("replay missing %s address=%016llx words=%zu\n",
                    clean ? "clean" : "ordinary", static_cast<unsigned long long>(address),
                    values.size());
        return false;
      }
      values[index] = found->second;
    }
    return true;
  };
  // Noncapturing function-pointer callbacks use a shared local adapter.
  struct Reader {
    decltype(read)* callback;
    Reads* capture;
    bool clean;
    static bool Invoke(void* userdata, uint64_t address, std::span<uint32_t> values) {
      auto& self = *static_cast<Reader*>(userdata);
      return (*self.callback)(*self.capture, self.clean, address, values);
    }
  } ordinary {&read, &reads, false}, clean {&read, &reads, true};
  const SrtRuntime runtime {.user_data = user_data,
                            .read_memory = Reader::Invoke,
                            .userdata = &ordinary,
                            .read_specialization_memory = Reader::Invoke,
                            .specialization_userdata = &clean};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  const bool ok = MaterializeResources(plan, runtime, snapshot, specialization);
  const bool complete = reads.missing == 0;
  std::printf("compiled replay materialized=%d missing=%u low-address-reads=%u\n", ok,
              reads.missing, reads.low);
  ResourceSnapshot reference_snapshot;
  ResourceSpecialization reference_specialization;
  reads.missing = reads.low = 0;
  const bool reference_ok = MaterializeResourcesReference(plan, runtime, reference_snapshot,
                                                         reference_specialization);
  std::printf("reference replay materialized=%d missing=%u low-address-reads=%u\n",
              reference_ok, reads.missing, reads.low);
  if (!complete || reads.missing != 0) {
    std::printf("Capture incomplete: missing reads are unknown, never synthesized as zeros.\n");
    return 1;
  }
  Check(ok == reference_ok, "compiled/reference materialization results differ");
  if (ok) {
    Check(snapshot.buffers == reference_snapshot.buffers &&
              snapshot.images == reference_snapshot.images &&
              snapshot.samplers == reference_snapshot.samplers &&
              snapshot.flattened_srt == reference_snapshot.flattened_srt &&
              snapshot.user_data == reference_snapshot.user_data &&
              snapshot.uniform_fill == reference_snapshot.uniform_fill &&
              specialization == reference_specialization,
          "compiled/reference materialization outputs differ");
  }
  return ok ? 0 : 1;
}
