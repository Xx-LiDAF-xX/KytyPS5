#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#if defined(_WIN32)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#if defined(__MINGW32__)
#include <pthread.h>
#endif
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#elif defined(__APPLE__) && defined(__MACH__)
#include <sys/sysctl.h>
#else
#error UniversalProcessorRouter supports Windows, Linux and macOS only.
#endif

#include "UniversalProcessorRouter.h"
#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

namespace {
#if !defined(_WIN32)
std::error_code GenericError(int code) {
    return {code, std::generic_category()};
}
#endif

#if defined(_WIN32)
std::error_code WindowsError() {
    return {static_cast<int>(GetLastError()), std::system_category()};
}
template<class Handle>
HANDLE WindowsHandle(Handle handle) {
    if constexpr (std::is_same_v<Handle, HANDLE>) {
        return handle;
    } else {
#if defined(__MINGW32__)
        return static_cast<HANDLE>(pthread_gethandle(handle));
#else
        static_assert(std::is_same_v<Handle, HANDLE>, "Unsupported Windows thread library");
#endif
    }
}
#elif defined(__linux__)
class CpuSet final {
public:
    explicit CpuSet(std::size_t count)
        : bytes(CPU_ALLOC_SIZE(count)), set(CPU_ALLOC(count)) {
        if (!set) throw std::bad_alloc();
        CPU_ZERO_S(bytes, set);
    }
    ~CpuSet() { CPU_FREE(set); }
    CpuSet(const CpuSet&) = delete;
    CpuSet& operator=(const CpuSet&) = delete;
    std::size_t bytes;
    cpu_set_t* set;
};

std::vector<std::uint32_t> AllowedCpus() {
    const long configured = sysconf(_SC_NPROCESSORS_CONF);
    std::size_t capacity = configured > 0 ? static_cast<std::size_t>(configured) : 128;
    for (;;) {
        CpuSet allowed(capacity);
        if (sched_getaffinity(0, allowed.bytes, allowed.set) == 0) {
            std::vector<std::uint32_t> cpus;
            // CPU_ALLOC_SIZE rounds up: inspect every bit the kernel can return.
            const std::size_t bits = allowed.bytes * 8;
            for (std::size_t cpu = 0; cpu < bits; ++cpu) {
                if (CPU_ISSET_S(cpu, allowed.bytes, allowed.set)) {
                    cpus.push_back(static_cast<std::uint32_t>(cpu));
                }
            }
            return cpus;
        }
        const int error = errno;
        if (error != EINVAL) throw std::system_error(GenericError(error), "sched_getaffinity");
        if (capacity > static_cast<std::size_t>(std::numeric_limits<int>::max()) / 2) {
            throw std::runtime_error("CPU affinity mask is too large");
        }
        capacity *= 2;
    }
}
long long ReadNumber(const std::string& path, bool required) {
    std::ifstream file(path);
    long long value = -1;
    if (file >> value && value >= 0) return value;
    if (required) throw std::runtime_error("CPU topology unavailable: " + path);
    return -1;
}
#elif defined(__APPLE__) && defined(__MACH__)
std::uint32_t SysctlCount(const char* name) {
    int value = 0;
    std::size_t bytes = sizeof(value);
    if (sysctlbyname(name, &value, &bytes, nullptr, 0) != 0) {
        throw std::system_error(GenericError(errno), name);
    }
    if (bytes != sizeof(value) || value <= 0) {
        throw std::runtime_error(std::string("Invalid CPU topology: ") + name);
    }
    return static_cast<std::uint32_t>(value);
}
#endif
} // namespace

UniversalProcessorRouter::UniversalProcessorRouter() {
#if defined(_WIN32)
    DWORD length = 0;
    if (GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        throw std::system_error(WindowsError(), "GetLogicalProcessorInformationEx size");
    }
    // max_align_t storage keeps variable-length Windows records properly aligned.
    std::vector<std::max_align_t> storage;
    for (;;) {
        storage.resize((length + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
        auto* data = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(storage.data());
        if (GetLogicalProcessorInformationEx(RelationProcessorCore, data, &length)) break;
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            throw std::system_error(WindowsError(), "GetLogicalProcessorInformationEx");
        }
    }
    auto* bytes = reinterpret_cast<unsigned char*>(storage.data());
    for (std::size_t offset = 0; offset < length;) {
        auto* record = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(bytes + offset);
        if (length - offset < offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Processor) ||
            record->Size == 0 || record->Size > length - offset) {
            throw std::runtime_error("Invalid Windows CPU topology record");
        }
        if (record->Relationship == RelationProcessorCore) {
            PhysicalCore core;
            core.performanceScore = record->Processor.EfficiencyClass;
            for (WORD group = 0; group < record->Processor.GroupCount; ++group) {
                const auto& mask = record->Processor.GroupMask[group];
                for (unsigned bit = 0; bit < sizeof(KAFFINITY) * 8; ++bit) {
                    if ((mask.Mask & (KAFFINITY{1} << bit)) != 0) {
                        core.siblings.push_back({mask.Group, bit});
                    }
                }
            }
            if (!core.siblings.empty()) cores_.push_back(std::move(core));
        }
        offset += record->Size;
    }
#elif defined(__linux__)
    // Respect the boot thread's cpuset/affinity. A later change requires a new router.
    std::map<std::tuple<long long, long long, long long>, std::size_t> ids;
    bool allHaveCapacity = true;
    for (const auto cpu : AllowedCpus()) {
        const std::string root = "/sys/devices/system/cpu/cpu" + std::to_string(cpu);
        const auto package = ReadNumber(root + "/topology/physical_package_id", true);
        const auto die = ReadNumber(root + "/topology/die_id", false);
        const auto coreId = ReadNumber(root + "/topology/core_id", true);
        const auto [entry, inserted] = ids.emplace(std::make_tuple(package, die, coreId), cores_.size());
        if (inserted) cores_.emplace_back();
        auto& core = cores_.at(entry->second);
        core.siblings.push_back({0, cpu});
        const auto capacity = ReadNumber(root + "/cpu_capacity", false);
        allHaveCapacity = allHaveCapacity && capacity > 0;
        if (capacity > 0) {
            core.performanceScore = std::max(core.performanceScore, static_cast<std::uint64_t>(capacity));
        }
    }
    // No marketing-name, clock-frequency or SMT-count guesses on hybrid hosts.
    if (!allHaveCapacity) for (auto& core : cores_) core.performanceScore = 0;
#elif defined(__APPLE__) && defined(__MACH__)
    physicalCount_ = SysctlCount("hw.physicalcpu");
    logicalCount_ = SysctlCount("hw.logicalcpu");
    // macOS exports counts, not a public logical-to-physical CPU binding map.
    return;
#endif
    if (cores_.empty()) throw std::runtime_error("No physical cores discovered");
    physicalCount_ = static_cast<std::uint32_t>(cores_.size());
    std::uint64_t highest = 0;
    for (const auto& core : cores_) {
        logicalCount_ += static_cast<std::uint32_t>(core.siblings.size());
        highest = std::max(highest, core.performanceScore);
    }
    for (std::uint32_t index = 0; index < physicalCount_; ++index) {
        if (cores_[index].performanceScore == highest) performance_.push_back(index);
    }
}

std::uint32_t UniversalProcessorRouter::PhysicalCoreCount() const noexcept { return physicalCount_; }
std::uint32_t UniversalProcessorRouter::LogicalProcessorCount() const noexcept { return logicalCount_; }
bool UniversalProcessorRouter::SupportsStrictAffinity() const noexcept {
#if defined(__APPLE__) && defined(__MACH__)
    return false;
#else
    return true;
#endif
}
const std::vector<UniversalProcessorRouter::PhysicalCore>&
UniversalProcessorRouter::PhysicalCores() const noexcept { return cores_; }
const std::vector<std::uint32_t>& UniversalProcessorRouter::PerformanceCoreIndices() const noexcept {
    return performance_;
}

UniversalProcessorRouter::PinResult UniversalProcessorRouter::PinThreadToPerformanceCore(
    std::thread::native_handle_type threadHandle, std::uint32_t coreIndex) const {
    PinResult result;
#if defined(__APPLE__) && defined(__MACH__)
    (void)threadHandle;
    (void)coreIndex;
    result.affinityError = std::make_error_code(std::errc::operation_not_supported);
    result.priorityError = std::make_error_code(std::errc::operation_not_supported);
    return result;
#else
    result.priorityError = std::make_error_code(std::errc::operation_canceled);
    if (coreIndex >= performance_.size()) {
        result.affinityError = std::make_error_code(std::errc::invalid_argument);
        return result;
    }
    const auto cpu = cores_.at(performance_[coreIndex]).siblings.front();
#if defined(_WIN32)
    const HANDLE handle = WindowsHandle(threadHandle);
    if (!handle) {
        result.affinityError = std::make_error_code(std::errc::bad_file_descriptor);
        return result;
    }
    GROUP_AFFINITY affinity{};
    affinity.Group = static_cast<WORD>(cpu.group);
    affinity.Mask = KAFFINITY{1} << cpu.id;
    if (!SetThreadGroupAffinity(handle, &affinity, nullptr)) {
        result.affinityError = WindowsError();
        return result;
    }
    result.affinityApplied = true;
    if (!SetThreadPriority(handle, THREAD_PRIORITY_TIME_CRITICAL)) {
        result.priorityError = WindowsError();
        return result;
    }
#elif defined(__linux__)
    CpuSet affinity(static_cast<std::size_t>(cpu.id) + 1);
    CPU_SET_S(cpu.id, affinity.bytes, affinity.set);
    const int affinityError = pthread_setaffinity_np(threadHandle, affinity.bytes, affinity.set);
    if (affinityError != 0) {
        result.affinityError = GenericError(affinityError);
        return result;
    }
    result.affinityApplied = true;
    sched_param priority{};
    priority.sched_priority = sched_get_priority_max(SCHED_FIFO);
    if (priority.sched_priority == -1) {
        result.priorityError = GenericError(errno);
        return result;
    }
    const int priorityError = pthread_setschedparam(threadHandle, SCHED_FIFO, &priority);
    if (priorityError != 0) {
        result.priorityError = GenericError(priorityError);
        return result;
    }
#endif
    result.priorityApplied = true;
    result.priorityError.clear();
    return result;
#endif
}
