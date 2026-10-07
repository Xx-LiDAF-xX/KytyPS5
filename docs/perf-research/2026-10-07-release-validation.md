# KYTYPS5 PERFORMANCE ANALYSIS — Release validation

## 1. Scope
Commit the authorized changes and publish a minimal Windows prerelease after a
full configured regression run and privacy review.

## 2. Build identity
The manifest records the clean source revision and packaging revision.
No local directory, tester identity, or configuration profile is included.

## 3. Hardware
Validation used an RTX 4060 Ti and Ryzen 7 5800X Windows host. Results do not
establish compatibility or performance on other hosts.

## 4. Evidence
All configured CTest entries ran sequentially with a 90-second timeout.
Raw local test output is excluded from Git and the release ZIP.

## 5. Build result
The emulator, launcher, and complete configured regression targets built.
A stale nonexistent benchmark dispatch was removed from MemoryTrackerTests.
No functional regression assertion was removed.

## 6. Regression result
64 of 73 tests passed. Nine failed or timed out. RELEASE_NOTES.md lists each test
and observed failure. No assertion or synchronization was bypassed to pass tests.

## 7. Targeted coverage
Log privacy, RT settings, APR ordering, controller settings, haptics, and focused
shader RT coverage passed. Prior APR stress and startup checks are documented in
the individual investigation reports.

## 8. Open correctness issues
Failures concern sockets, audio priming, register classification, command lane
progress, tiling scratch reuse, and GPU metadata clears. Causes and game impact
need separate investigation; publication does not resolve them.

## 9. Privacy scan
Current tracked and new files were scanned for host identities, profile paths,
credential patterns and private keys. Qt parser header constants were distinguished
from actual key material. Runtime binaries exclude private host paths.

## 10. Repository cleanup
A personal path in a research note was removed. Obsolete tracked release ZIPs
were removed from the index and ignored, with local copies preserved. Existing
Git history and remote historical release assets were not rewritten.

## 11. Runtime packaging
An explicit allowlist includes executables, runtime DLLs and Qt plugins.
Fresh portable settings, README, release notes, licenses and a hash manifest are
included. Game data, saves, logs, caches, patches and PDB files are excluded.

## 12. Dependency privacy
Signed Qt DLLs contain standard vendor build/debug paths. Modifying them would
invalidate their signatures, so these vendor strings are retained and disclosed.
They are not developer or tester installation paths.

## 13. Measurement limits
No FPS improvement is claimed. Short prior Astro startup coverage is not a full
gameplay soak. Unresolved assertions require this build to remain a prerelease.

## 14. Reproducibility
tools/package-release.py generates the runtime archive, hash manifest and checksum.
Source and package revisions are recorded independently so the embedded build
identity can be verified against published source.
