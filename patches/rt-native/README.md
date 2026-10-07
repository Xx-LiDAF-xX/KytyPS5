# Native RT/GI profiles and trigger gameplay fix

These profiles disable every bypass in the supplied game patches. They preserve
the game's own renderer and GI; they do not force an arbitrary game flag to enable
RT. The matching game dumps in C:\ROMS were verified by SHA256.

Select the matching JSON in the launcher and enable its RT setting, or run:

```powershell
.\tools\run-native-rt.ps1 -TitleId PPSA21567
.\tools\run-native-rt.ps1 -TitleId PPSA01325
```

The script checks the game hash, enables emulator RT, disables automatic tuning
and uses the installed emulator's existing run/save directory. Use `-RunDirectory`
for a separate test directory. Original game files and patch files are retained.
Copies of these JSON files are in Desktop\_Patches\RT-native-trigger-fix.

The L2/R2 frog-glove correction is in the emulator. Previously the trigger-state
API returned zero even when the game configured a weapon/feedback effect and the
player pressed a trigger. The updated implementation derives its state from the
accepted effect and input travel, including partial and digital presses. This is
an approximation of the controller motor state because SDL does not expose that
physical report. Host output intensity does not alter guest gameplay state.

Repeated unchanged effects already delivered to the host are not resent. Failed
sends invalidate only the selected trigger's delivery cache and can be retried.

The updated renderer keeps guarded RT pointer chains on the GPU, clearing the
reproduced startup descriptor-read crash. Longer Astro Bot tests expose a separate
graphics shader-interface failure after the startup video. These profiles remain
experimental and must not be described as optimized or crash-free RT patches.
The latest mask-register correction clears the reported `0xA34` image-descriptor
translation error. A 92-second RT-enabled startup run completed its bounded test
without that error; interactive gameplay and sustained 60 FPS remain unverified.
See `docs/perf-research/2026-10-07-rt-image-mask-pairs.md` for evidence.
The trigger tests pass; frog-glove gameplay still requires confirmation in Aztec
Trail. See the guarded-buffer analysis in docs/perf-research for test evidence.

Trigger-state logic was adapted from [Jetsku/KytyPS5's adaptive trigger fix](https://github.com/Jetsku/KytyPS5/commit/a6d70665df26c1ea1d0e2256b37e474b9a53b66e)
and subsequent feedback/vibration corrections bbe1f24b and 6a8a89ee. Invalid
commands remain rejected rather than changing the reported guest state.
