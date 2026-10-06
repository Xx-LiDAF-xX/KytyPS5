# UniversalProcessorRouter

Complete standalone C++20 library and runnable example; no emulator integration or automatic affinity changes.
Construct the router during boot, before narrowing the boot thread's affinity. The constructor snapshots
topology, and throws on discovery failure. CPU hotplug or cpuset changes require reconstruction.

Build on Windows, Linux or macOS:

```sh
cmake -S tools/universal_processor_router -B build/router
cmake --build build/router --config Release
```

The executable is `universal_processor_router_demo` (with `.exe` on Windows); multi-configuration
generators place it under `Release`. Without arguments it only prints topology. Pass `--pin` to
pin a waiting worker and request maximum thread priority; the worker exits immediately afterwards.

## Platform contract

Windows uses `GetLogicalProcessorInformationEx(RelationProcessorCore)` and preserves processor-group
IDs, enumerating logical siblings per physical core. Highest `EfficiencyClass` cores form the performance
candidate list; a homogeneous CPU includes all cores. The method pins to one sibling using
`SetThreadGroupAffinity` and requests `THREAD_PRIORITY_TIME_CRITICAL`, the maximum relative thread
priority within the existing process priority class. It deliberately does not change the priority class
of the entire process; absolute system priority 31 would additionally require a realtime process class.
MSVC/clang-cl native handles and MinGW-w64 winpthreads handles are supported. Use a 64-bit Windows
build: WOW64 cannot represent all 64 processors of a processor group accurately with this topology API.
Process/job/CPU-set restrictions may cause a selected core to be unavailable; the actual API error is returned.

Linux snapshots the boot thread's allowed CPU mask, respecting cpusets/affinity restrictions. It reads
package/die/core IDs from sysfs to group available SMT siblings. Counts describe that allowed subset.
Dynamic masks support CPU IDs beyond `CPU_SETSIZE`. It uses `cpu_capacity` for ranking only when all
available CPUs expose a positive value. Otherwise it reports no ranking and treats all physical cores as
candidates; this does not claim to identify P cores on a hybrid host lacking capacity metadata.
Required topology files must be visible; masked sysfs produces a discovery error rather than guessing.
The method pins one logical processor with `pthread_setaffinity_np`, then requests `SCHED_FIFO` at
`sched_get_priority_max(SCHED_FIFO)`. Permission failures are reported, and the affinity remains applied.

macOS reads `hw.physicalcpu` and `hw.logicalcpu` through `sysctlbyname`. Public APIs do not expose
strict physical-core binding or a logical-to-physical map. `SupportsStrictAffinity()` is false; core-map
and performance-candidate vectors are empty. `PinThreadToPerformanceCore` returns
`operation_not_supported` for both operations and modifies nothing. Mach affinity tags are cache-sharing
hints, not CPU numbers; a QoS request also does not guarantee a particular core. This is an explicit
platform limitation, not a simulated implementation of physical binding.

## API semantics

`coreIndex` indexes `PerformanceCoreIndices()`, which in turn indexes `PhysicalCores()`; it is not an OS
logical CPU number. Selecting one logical processor of each physical core prevents this router's selected
threads from sharing the same core when callers choose distinct indices. It does not reserve cores,
disable hyperthreads or prevent other processes from using the sibling. Callers own the native handle,
keep the worker alive, and serialize changes to the same thread. After affinity succeeds but priority
fails, `affinityApplied` is true and `priorityApplied` false. No implicit fallback or rollback occurs.
Allocation can throw `std::bad_alloc`. The router's immutable snapshot may be used concurrently for
different threads. Priority is requested only after successful affinity.

The exact requested combination cannot be guaranteed on all three operating systems: macOS lacks the
binding API, Linux may lack permission, and Windows has process-relative scheduling priority. Inspect
`PinResult` rather than assuming that compilation implies both operations succeeded.

## Verification on October 7, 2026

Compiled with MinGW-w64 GCC 15.2, C++20, `-Wall -Wextra -Werror -pthread`; the standalone CMake
Release build also succeeds. On the Ryzen 7 5800X host, discovery reports 8 physical cores and 16
logical processors, grouped into 8 pairs. The `--pin` example rejects an invalid index and successfully
applies affinity and time-critical relative priority to its waiting worker, then exits normally.
Linux/macOS builds and execution are unverified on this Windows-only host. For MinGW, put the matching
compiler's `bin` directory on PATH to locate its runtime DLLs; avoid launching from a directory holding
incompatible DLLs from a different GCC installation.

Primary references:

- https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-getlogicalprocessorinformationex
- https://learn.microsoft.com/en-us/windows/win32/api/processtopologyapi/nf-processtopologyapi-setthreadgroupaffinity
- https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-processor_relationship
- https://learn.microsoft.com/en-us/windows/win32/procthread/scheduling-priorities
- https://man7.org/linux/man-pages/man3/pthread_setaffinity_np.3.html
- https://man7.org/linux/man-pages/man3/pthread_setschedparam.3.html
- https://developer.apple.com/library/archive/releasenotes/Performance/RN-AffinityAPI/index.html
