#include "UniversalProcessorRouter.h"
#include <future>
#include <iostream>
#include <limits>
#include <string_view>

int main(int argc, char** argv) {
    try {
        UniversalProcessorRouter router;
        std::cout << "Physical cores: " << router.PhysicalCoreCount()
                  << "\nLogical processors: " << router.LogicalProcessorCount()
                  << "\nStrict affinity supported: " << router.SupportsStrictAffinity()
                  << "\nPerformance candidates: " << router.PerformanceCoreIndices().size() << '\n';
        for (std::size_t index = 0; index < router.PhysicalCores().size(); ++index) {
            const auto& core = router.PhysicalCores()[index];
            std::cout << "Core " << index << " score=" << core.performanceScore << " logical:";
            for (const auto cpu : core.siblings) std::cout << ' ' << cpu.group << ':' << cpu.id;
            std::cout << '\n';
        }
        if (argc == 1) return 0;
        if (argc != 2 || std::string_view(argv[1]) != "--pin") {
            std::cerr << "Usage: universal_processor_router_demo [--pin]\n";
            return 2;
        }
        std::promise<void> release;
        auto ready = release.get_future();
        std::jthread worker([&ready] { ready.wait(); });
        UniversalProcessorRouter::PinResult result;
        try {
            const auto invalid = router.PinThreadToPerformanceCore(
                worker.native_handle(), std::numeric_limits<std::uint32_t>::max());
            if (invalid.affinityApplied || invalid.priorityApplied) {
                throw std::runtime_error("Invalid core index unexpectedly modified the worker");
            }
            result = router.PinThreadToPerformanceCore(worker.native_handle(), 0);
        } catch (...) {
            release.set_value();
            throw;
        }
        release.set_value();
        worker.join();
        std::cout << "Affinity applied: " << result.affinityApplied
                  << "; priority applied: " << result.priorityApplied << '\n';
        if (result.affinityError) std::cerr << "Affinity: " << result.affinityError.message() << '\n';
        if (result.priorityError) std::cerr << "Priority: " << result.priorityError.message() << '\n';
        return result.Succeeded() ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
