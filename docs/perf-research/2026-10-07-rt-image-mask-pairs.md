# KYTYPS5 PERFORMANCE ANALYSIS

## 1. CURRENT PERFORMANCE

The new pasted Astro Bot output reaches the startup video, then fails translating
compute shaders `280e6a01e6b640f7` and `8bcbd7892939ad62`, at native PC `0xA34`.
Desktop `_kyty.txt` is an older file containing the previous `0x30` failure; its
timestamp is 11:25:42. The installed executable was verified by SHA256.

After the mask correction, a bounded Astro Bot run lasted 92.3 seconds with RT
enabled and the user's asynchronous pipeline options, without the reported shader
error or a fatal log entry. The process was deliberately stopped at the test limit.
It reached at least frame 3256, but showed a 783 ms shader/pipeline compilation
hitch. This is not evidence of sustained 60 FPS or verified interactive gameplay.

## 2. BOTTLENECK CLASSIFICATION

Incorrect scalar register mask addressing corrupts an image descriptor in shader IR.
This is a correctness defect rather than a measured frame-rate bottleneck.

## 3. EVIDENCE

The captured shader is
`_Build/rt/probes/image-source-20261007-115342/_Shaders/original/0159_new_shader_cs_280e6a01e6b640f7.bin`.
Native PC `0x95C` loads the image into s[4:11]. At `0xA10`, the comparison mask's
encoded destination is 3. Before correction, its high ballot word overwrites s4;
the gather at `0xA34` consequently receives a ballot as descriptor word zero.
The pre-tracking IR shows this directly: `%1988 = GetImageResource %1973, ...`,
where `%1973` is the high component of a ballot. Fixed offline translation clears
that failure. Materialization replay remains incomplete because eight reads were
not captured; unknown data is not synthesized.

## 4. ROOT CAUSE

Wave64 scalar masks address aligned SGPR pairs. The translator treated the odd
encoding as the low register of an unaligned pair. Mask reads and validity tags
also used that unaligned register. LLVM's AMDGPU disassembler maps a 64-bit scalar
register encoding through a one-bit shift into its pair register class, while
32-bit registers do not use the shift.
[LLVM implementation](https://github.com/llvm/llvm-project/blob/main/llvm/lib/Target/AMDGPU/Disassembler/AMDGPUDisassembler.cpp)

## 5. PROPOSED FIX

Normalize wave64 mask reads and writes to the even register below the encoded
number. Apply the same normalization to mask tags. Preserve wave32 single-register
semantics and align explicit 64-bit mask writes in either wave mode.

## 6. FILES/FUNCTIONS TO MODIFY

`frontend/translate/Translate.cpp`: `ReadMask`, `ReadMaskValid`, `WriteMask`.
`tests/ScalarMaskPairCases.inc` and the GPU test runner add direct mask tests.
The captured SRT replay now enables its existing early IR dump for diagnosis.

## 7. PATCH

The mask's SGPR number is aligned with `& ~1u` when the access is 64-bit.
Scalar-write metadata records the registers actually written. No image operation
is skipped, and descriptor validation remains strict.

## 8. WHY THIS SHOULD HELP

The comparison writes s[2:3], preserving the descriptor beginning at s4. The gather
then uses the descriptor originally loaded by the guest shader. Mask consumers
read the same pair the producer wrote.

## 9. CORRECTNESS RISKS

Wave32 must retain its odd single-register destination. Wave64 readers must use
both low and high words of the aligned pair. GPU tests check the destination words,
an adjacent sentinel, and per-lane conditional-mask results in both wave modes.

## 10. VENDOR IMPACT

Validated on the local RTX 4060 Ti. This is guest shader translation, not a new
vendor-specific hardware RT pipeline. AMD/Intel hardware is untested.

## 11. TEST PROCEDURE

Build emulator, shader CFG tests, and compute tests. Run the eight selected CTest
regressions, `--scalar-mask-pair-only`, and `--restored-compare-only`. Run Astro Bot
with RT on and the user's asynchronous pipeline/library options. That reproduction
uses Vulkan validation off to match the reported run; it does not resolve or excuse
the separate vertex/fragment interface error found with validation enabled.

## 12. ACCEPTANCE CRITERIA

Both direct mask tests pass, the six existing comparison GPU cases pass, and all
eight selected CTest regressions pass. The captured shader no longer rejects its
image descriptor at `0xA34`. Report later game failures separately.

Runtime evidence is in `_Build/rt/probes/mask-pair-runtime-20261007-120138/`.
The tested executable SHA256 is
`982FA9BE1223CAFCC752828F9BCE93C134EA5183EEAAF7A1A663D4D1A8355038`.
The same binary was installed into repository `bin` and Desktop `Kytytest`;
previous executables are backed up under
`_Build/rt/backups/rt-mask-pair-20261007-120443/`. Deployment hashes are recorded
in `_Build/rt/rt-mask-pair-deployment.json`.

## 13. REGRESSION CHECK

The two direct GPU cases and six comparison cases pass. All eight selected CTest
regressions pass. The original pointer-chain repair and controller corrections are
retained. No frame-rate minimum or interactive gameplay result is claimed.

## 14. NEXT BOTTLENECK

The known vertex/fragment interface mismatch remains under strict Vulkan validation.
Continue from the bounded runtime probe's actual result rather than assuming all RT
effects or later gameplay are correct.

The bounded run also exposes pipeline compilation stalls; fixing those must retain
shader correctness and real GPU timing. Desktop timestamp scaling was found at
125 and restored to 100, with its prior settings backed up before editing.
