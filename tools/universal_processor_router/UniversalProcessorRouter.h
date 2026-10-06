#pragma once

#include <cstdint>
#include <system_error>
#include <thread>
#include <vector>

class UniversalProcessorRouter final {
public:
    struct LogicalProcessor {
        std::uint32_t group;
        std::uint32_t id;
    };
    struct PhysicalCore {
        std::vector<LogicalProcessor> siblings;
        std::uint64_t performanceScore = 0;
    };
    struct PinResult {
        bool affinityApplied = false;
        bool priorityApplied = false;
        std::error_code affinityError;
        std::error_code priorityError;
        [[nodiscard]] bool Succeeded() const noexcept {
            return affinityApplied && priorityApplied;
        }
    };

    // Construct on the boot thread, before imposing affinity restrictions.
    // Discovery errors throw std::system_error/std::runtime_error.
    UniversalProcessorRouter();
    [[nodiscard]] std::uint32_t PhysicalCoreCount() const noexcept;
    [[nodiscard]] std::uint32_t LogicalProcessorCount() const noexcept;
    [[nodiscard]] bool SupportsStrictAffinity() const noexcept;
    [[nodiscard]] const std::vector<PhysicalCore>& PhysicalCores() const noexcept;
    // Indices into PhysicalCores(). Highest reported performance class only;
    // when the OS supplies no ranking, all discovered cores are candidates.
    [[nodiscard]] const std::vector<std::uint32_t>& PerformanceCoreIndices() const noexcept;

    // coreIndex indexes PerformanceCoreIndices(), not an OS logical CPU number.
    // Pins to one logical processor belonging to the selected physical core.
    // Does not reserve that core or disable its other SMT siblings.
    // The handle must remain alive; serialize changes to the same thread.
    [[nodiscard]] PinResult PinThreadToPerformanceCore(
        std::thread::native_handle_type threadHandle, std::uint32_t coreIndex) const;

private:
    std::vector<PhysicalCore> cores_;
    std::vector<std::uint32_t> performance_;
    std::uint32_t physicalCount_ = 0;
    std::uint32_t logicalCount_ = 0;
};
