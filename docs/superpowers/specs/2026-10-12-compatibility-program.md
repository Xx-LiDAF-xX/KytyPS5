# KytyPS5 Compatibility Program Specification

## Vision

Develop the KytyPS5 fork into a Windows and Linux PlayStation 5 translation layer whose supported
titles behave like stable native PC games: correct rendering and audio, responsive input, reliable
saves, deterministic startup, smooth frame delivery, and no emulator-originated crashes during a
verified playthrough.

"Perfect" is a certification target for a defined game version and host matrix, not an unsupported
claim about every title and computer. Progress is measured by reproducible evidence and retained
regression coverage.

## Initial Host Baseline

- Primary operating systems: Windows and Linux.
- Reference CPU: AMD Ryzen 7 5800X.
- Reference GPU: NVIDIA GeForce RTX 4060 Ti with 16 GB VRAM.
- Reference memory: 32 GB DDR4-3200.
- Graphics interface: Vulkan 1.3.
- A physical PlayStation 5 owned by the project operator is available for lawful clean-room output
  and timing comparisons.
- AMD and Intel GPU behavior remains an explicit portability requirement, but it cannot be certified
  from the reference machine alone.

## Initial Game Corpus

The local `sce_sys/param.json` content is the authoritative source for each title's name, title ID,
content version, master version, SDK metadata, and region. Expected title names are listed only to
make the starting corpus readable.

| Title ID | Expected title |
| --- | --- |
| PPSA01325 | Astro's Playroom |
| PPSA01859 | Resident Evil Village: Maiden |
| PPSA02100 | Stray |
| PPSA21567 | Astro Bot |
| PPSA25991 | Senua's Saga: Hellblade II |
| PPSA26223 | The Mound: Omen of Cthulhu |
| PPSA27624 | Bendy and the Dark Revival |
| PPSA31162 | Super Meat Boy 3D |

Game directories, executables, patches, keys, captured proprietary payloads, save data, and other
copyrighted material must remain outside Git. Committed reports may contain hashes, metadata,
project-authored test inputs, diagnostics, and reproduction instructions that do not redistribute
protected content.

## Compatibility Rungs

1. **Rung 0 — Loads:** the executable is accepted and reaches guest execution.
2. **Rung 1 — Renders:** a stable visible frame is produced.
3. **Rung 2 — Menu:** the title reaches an interactive menu with working input and audio.
4. **Rung 3 — Gameplay:** a documented route reaches controllable gameplay.
5. **Rung 4 — Playable:** a substantial session works without a blocking defect.
6. **Rung 5 — Complete:** a full playthrough, saves, reload, audio, input, and major modes are verified.
7. **Rung 6 — Console-equivalent:** Rung 5 plus correct output, stable frame pacing, no emulator
   validation errors, and repeatability on the declared Windows/Linux host matrix.

Every claim records the KytyPS5 commit, game version, executable hash, operating system, GPU driver,
configuration, reproduction route, duration, and observed failure or success.

## Program Workstreams

The program is split into separately planned workstreams:

1. Compatibility foundation and deterministic diagnostics.
2. GPU command, shader, resource, and memory semantics.
3. Loader, ABI, kernel, and guest-memory fidelity.
4. Audio, video, input, storage, dialogs, and system-library fidelity.
5. Performance, frame pacing, cache behavior, and presentation quality.
6. Cross-vendor and cross-platform certification.

The first workstream must land before broad title fixes so each discovered defect becomes a retained
test or replay case instead of a one-off patch.

## Compatibility Foundation Requirements

- An opt-in structured JSON report records build, host, title, configuration, selected Vulkan device,
  lifecycle milestones, unsupported imports, fatal errors, and final outcome.
- Diagnostics remain safe when disabled and do not change guest-visible behavior.
- Fatal reporting is best-effort and must not replace the original text log or obscure the original
  failure.
- A local corpus manifest stores game paths outside source control and identifies games by title ID.
- A corpus runner executes one title at a time with a bounded timeout, isolated output directory,
  deterministic configuration, and preserved stdout/stderr.
- A summarizer produces machine-readable results and a Markdown baseline table without copying game
  payloads into the repository.
- CTest cases are classified so CPU-only tests run everywhere and Vulkan-dependent tests run only on
  capable workers.
- Windows and Linux use the same report schema and corpus manifest format.
- PS5 comparisons follow a written observation protocol covering scene, game settings, display mode,
  controller input sequence, elapsed time, screenshot/video evidence, and known sources of variance.

## Phase-One Acceptance Criteria

- Unit tests prove stable JSON serialization, disabled-mode behavior, concurrent event recording,
  fatal-event preservation, and path sanitization.
- Python tests prove manifest validation, timeout handling, process exit classification, output
  isolation, and summary generation.
- Windows and Linux CI run every test labeled `core` and publish failures with test output.
- A local dry run can exercise a fake executable without any game files.
- Each of the eight titles has a sanitized baseline entry generated from its exact local metadata.
- No copyrighted payload or absolute user path appears in a committed artifact.

## Legal and Safety Constraints

- Use only software, hardware, dumps, and captures the project operator is legally permitted to use.
- Do not distribute Sony firmware, SDK material, cryptographic keys, game executables, or game assets.
- Do not bypass third-party access controls, enable piracy, cheat in online play, or interfere with
  PlayStation or game services.
- Preserve the repository's GPL-2.0-only license obligations, notices, and third-party licenses.
- Prefer public specifications and clean-room behavioral observation. Record provenance for external
  technical facts used by an implementation.

## Deferred From Phase One

- Fixing individual game compatibility defects.
- Automatic controller scripting inside the emulator.
- Committing captured shaders or command buffers from commercial games.
- Cross-vendor certification without access to the corresponding hardware.
- Performance or image-quality enhancement features beyond accurate baseline measurement.
