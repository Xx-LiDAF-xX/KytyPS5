#include "common/slotVector.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <vector>

namespace {

void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "SlotVectorTests: failed: %s\n", message);
		std::exit(1);
	}
}

struct Resource {
	explicit Resource(uint64_t v): value(v) { ++live; }
	~Resource() { --live; }
	Resource(const Resource&)                           = delete;
	Resource&                operator=(const Resource&) = delete;
	uint64_t                 value;
	std::array<uint64_t, 32> padding {};
	inline static size_t     live = 0;
};

using Slots = Common::SlotVector<Resource>;
static_assert(std::is_same_v<decltype(std::declval<const Slots&>().try_get({})), const Resource*>);
static_assert(std::is_same_v<decltype(std::declval<const Slots&>()[{}]), const Resource&>);

void TestLookupAndLifetime() {
	Slots                        slots;
	std::vector<Common::SlotId>  ids;
	std::vector<const Resource*> addresses;
	for (uint64_t i = 0; i < 4097; ++i) {
		ids.push_back(slots.insert(i));
		addresses.push_back(&slots[ids.back()]);
	}
	Check(slots.size() == ids.size() && Resource::live == ids.size(),
	      "resource count after growth");
	for (size_t i = 0; i < ids.size(); i += 3) {
		slots.erase(ids[i]);
	}
	for (bool dense: {false, true, false}) {
		Common::g_slot_vector_dense.store(dense, std::memory_order_relaxed);
		const auto& const_slots = slots;
		for (size_t i = 0; i < ids.size(); ++i) {
			const bool live = i % 3 != 0;
			Check(slots.is_allocated(ids[i]) == live, "allocation state in both lookup modes");
			Check(slots.try_get(ids[i]) == (live ? addresses[i] : nullptr),
			      "stable mutable lookup");
			Check(const_slots.try_get(ids[i]) == (live ? addresses[i] : nullptr),
			      "stable const lookup");
			if (live) {
				Check(slots[ids[i]].value == i && const_slots[ids[i]].value == i, "indexed value");
			}
		}
		for (auto invalid: {Common::SlotId {}, Common::SlotId {50000, 1}, Common::SlotId {1, 0},
		                    Common::SlotId {1, 99}}) {
			Check(!slots.is_allocated(invalid) && slots.try_get(invalid) == nullptr &&
			          const_slots.try_get(invalid) == nullptr,
			      "invalid handle");
		}
	}
	while (slots.size() < ids.size()) {
		const auto replacement = slots.insert(9000);
		Check(replacement.generation != ids[replacement.index].generation, "reused generation");
		for (bool dense: {false, true}) {
			Common::g_slot_vector_dense.store(dense, std::memory_order_relaxed);
			Check(slots.try_get(ids[replacement.index]) == nullptr, "stale handle after reuse");
			Check(slots[replacement].value == 9000, "replacement value");
		}
	}
	size_t visited = 0;
	std::as_const(slots).ForEach([&](Common::SlotId id, const Resource& resource) {
		Check(slots.try_get(id) == &resource, "iteration agrees with lookup");
		++visited;
	});
	Check(visited == slots.size(), "iteration count");
	slots.ForEach([&](Common::SlotId id, Resource&) { slots.erase(id); });
	Check(slots.size() == 0 && Resource::live == 0, "erase during iteration");
	Check(slots.capacity() == ids.size(), "reuse preserves capacity");
	for (uint64_t i = 0; i < 17; ++i) {
		(void)slots.insert(i);
	}
	Check(Resource::live == 17, "resources left for container destruction");
}

void Benchmark() {
	for (const size_t size: {4096u, 16384u}) {
		Slots                       slots;
		std::vector<Common::SlotId> ids;
		for (size_t i = 0; i < size; ++i) {
			ids.push_back(slots.insert(i));
		}
		for (bool dense: {false, true}) {
			Common::g_slot_vector_dense.store(dense, std::memory_order_relaxed);
			for (bool stale: {false, true}) {
				auto queries = ids;
				if (stale) {
					for (size_t i = 0; i < size; i += 2)
						++queries[i].generation;
				}
				std::array<double, 7> samples {};
				uint64_t              checksum   = 0;
				constexpr size_t      iterations = 4'000'000;
				for (auto& sample: samples) {
					const auto start = std::chrono::steady_clock::now();
					for (size_t i = 0; i < iterations; ++i) {
						const auto* resource =
						    slots.try_get(queries[(i * 2654435761u) & (size - 1)]);
						checksum += resource != nullptr ? resource->value : 1;
					}
					sample = std::chrono::duration<double, std::nano>(
					             std::chrono::steady_clock::now() - start)
					             .count() /
					         iterations;
				}
				std::sort(samples.begin(), samples.end());
				std::printf(
				    "slots=%zu dense=%d stale=%d median_ns=%.3f min_ns=%.3f checksum=%llu\n", size,
				    dense, stale, samples[3], samples[0],
				    static_cast<unsigned long long>(checksum));
			}
		}
	}
}

} // namespace

int main(int argc, char** argv) {
	TestLookupAndLifetime();
	Check(Resource::live == 0, "destruction releases resources");
	if (argc == 2 && std::strcmp(argv[1], "--benchmark") == 0) Benchmark();
	Common::g_slot_vector_dense.store(false, std::memory_order_relaxed);
	std::puts("SlotVectorTests: all cases passed");
}
