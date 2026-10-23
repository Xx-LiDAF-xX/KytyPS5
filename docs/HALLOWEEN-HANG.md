# Halloween post-intro command-buffer stall

Title: PPSA29340, version 01.011.000. Investigation starts at local commit
`c19b41d0`.

With the game's timeout/fence bypass patches disabled, a fresh build stopped
after 1,738 flips. The watchdog captured graphics submission 6956 waiting for
`0x2683` and compute submission 6955 waiting for `0x25d7` at the same address,
`0x107e68e3c0`, whose value stayed at `0x25d5`.

The graphics submission preceding these waits contained:

- A conditional indirect buffer at `0x10f7280484`, header `0xc00c3f00`,
  mode 1, comparison function 0 (always), and a 33-DWORD then target.
- An ordinary chained indirect buffer at `0x10f72804bc`, continuing at
  `0x10f7200300`.

`CpOpBranch` passed `chain=true` for the conditional target. This replaced
the parent cursor, so finishing the short target discarded the continuation
and its fence writes. Both queues then waited forever for work never parsed.
Conditional indirect buffers now retain the parent cursor. Ordinary indirect
buffers still honor their explicit CHAIN bit.

`CheckPm4IndirectControlFlow` checks both condition outcomes in both modes:
the selected target, a separate parent completion write, and the outer caller's
return must all be preserved. Its existing long-chain case retains coverage
of explicit chaining.

The two game-side wait/timeout bypass mods added in `c19b41d0` are disabled.
They remove guest synchronization without repairing the missing GPU work;
the enabled bypass baseline also failed to reach normal frame progress.
No guest executable on disk is modified by this fix.

Validation:

- The revised regression failed against the original handler with
  `conditional indirect buffer skipped its parent's completion write`.
- The Release emulator and graphics-test executable rebuilt successfully.
- All six CTest cases passed: `cp_seq_direct`, `cp_seq_inline`,
  `cp_seq_inline_verify`, `cp_seq_thread`, `cp_seq_thread_verify`, and
  `cp_seq_thread_prefetch_verify`.
