// KYTY_VRAM_GC_BUDGET marks and plans (graphics/host_gpu/vramBudget.h) for a 24 GB, a 12 GB and an
// 8 GB card's planning budgets.
#include "graphics/host_gpu/vramBudget.h"

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <limits>

namespace {

using namespace Libs::Graphics::VramBudget;

constexpr uint64_t MiB = 1024ull * 1024;
constexpr uint64_t GiB = 1024 * MiB;

int g_failures = 0;

void Expect(bool condition, const char* what) {
	if (!condition) {
		std::printf("VramBudgetTests: failed: %s\n", what);
		g_failures++;
	}
}

void TestMarks() {
	const uint64_t budget = 9 * GiB + 512 * MiB; // a 12 GB card's planning budget
	Expect(ImageTrigger(budget) == budget / 4 * 3, "image trigger is three quarters of the budget");
	Expect(Critical(budget) == budget - budget / 16, "critical mark 1/16 below the budget");
	Expect(BufferTrigger(budget) == budget, "buffer collector only at the budget");
	Expect(BufferCritical(budget) > budget, "buffer downloads only above the budget");
	Expect(ImageTrigger(budget) < Critical(budget), "trigger below the critical mark");
}

void TestPlanningBudgets() {
	Expect(PlanningBudget(10 * GiB, true, true) == 9 * GiB,
	       "discrete GPU retains the existing 1 GiB maximum reserve");
	Expect(PlanningBudget(4 * GiB, false, true) == 3584 * MiB,
	       "integrated GPU follows the driver budget instead of the physical heap size");
	Expect(PlanningBudget(16 * GiB, false, true, 2 * GiB) == 1792 * MiB,
	       "integrated GPU honors the user cap even with a large shared heap");
	Expect(PlanningBudget(8 * GiB, false, false) == 6 * GiB,
	       "shared-memory fallback reserves a proportion on smaller PCs");
	Expect(PlanningBudget(64 * GiB, false, false) == 56 * GiB,
	       "shared-memory fallback keeps the 8 GiB maximum reserve");
	for (const auto bytes: {uint64_t {0}, uint64_t {1}, 512 * MiB, 2 * GiB, 8 * GiB,
	                        32 * GiB, std::numeric_limits<uint64_t>::max()}) {
		for (const bool discrete: {false, true}) {
			for (const bool driver: {false, true}) {
				const auto budget = PlanningBudget(bytes, discrete, driver);
				Expect(budget <= bytes, "planning budget never exceeds available capacity");
				Expect(PlanningBudget(bytes, discrete, driver, GiB) <= std::min(bytes, GiB),
				       "cap never exceeded on any GPU or fallback path");
			}
		}
	}
}

void TestRetainedImageBudgets() {
	const auto large = RetainedImageLimit(GiB, 16 * GiB);
	const auto small = RetainedImageLimit(GiB, 2 * GiB);
	Expect(large == GiB && small == 256 * MiB, "pool capacity follows a shrinking GPU budget");
	Expect(RetainedImageLimit(64 * MiB, 16 * GiB) == 64 * MiB, "pool override remains an upper bound");
	Expect(RetainedImageLimit(GiB, 0) == 0, "zero budget retains no images");
	Expect(RetainedImageLimit(0, 16 * GiB) == 0, "explicit zero disables retention");
	Expect(RetainedImageFits(768 * MiB, 256 * MiB, large), "exact fit is reusable");
	Expect(!RetainedImageFits(768 * MiB, MiB, small), "shrinking budget cannot underflow free space");
	Expect(!RetainedImageFits(256 * MiB, MiB, small), "full pool rejects another image");
	Expect(!RetainedImageFits(0, 512 * MiB, small), "oversized image is not retained");
	Expect(RetainedImageFits(128 * MiB, 128 * MiB, small), "space reclaimed by trimming can be reused");
	Expect(!RetainedImageFits(std::numeric_limits<uint64_t>::max(), 1, small),
	       "large retained count cannot wrap the capacity check");
}

void TestPlans() {
	// 24 GB card, Sky Garden (9.6 GB): nothing to do (the stock trigger would be 7.3 GiB).
	const uint64_t large = 22 * GiB + 512 * MiB;
	Expect(!PlanImages(large, 9600 * MiB, 0).retire, "24 GB: no retirement at 9.6 GB");
	// 12 GB card: 5.8 GB (Sky Garden with KYTY_FUNCTION_ARRAY_SHRINK) is below the trigger.
	const uint64_t medium = 9 * GiB + 512 * MiB;
	Expect(!PlanImages(medium, 5800 * MiB, 0).retire, "12 GB: no retirement at 5.8 GB");
	const auto above = PlanImages(medium, 8 * GiB, 0);
	Expect(above.retire && above.age_frames == DefaultAgeFrames, "12 GB: 8 GiB retires 30-frame-old images");
	Expect(above.bytes == 8 * GiB - ImageTrigger(medium), "12 GB: down to the trigger");
	const auto critical = PlanImages(medium, 9 * GiB + 256 * MiB, 0);
	Expect(critical.retire && critical.age_frames == DefaultAgeFrames / 4,
	       "12 GB: above the critical mark a quarter of the age");
	// 8 GB card: 5.8 GB is above the trigger.
	const uint64_t small = 6 * GiB + 300 * MiB;
	const auto     tight = PlanImages(small, 5800 * MiB, 0);
	Expect(tight.retire && tight.age_frames == DefaultAgeFrames, "8 GB: 5.8 GB retires 30-frame-old images");
	// KYTY_VRAM_PRESSURE_FRAMES as the age, and its floor of 4 frames.
	Expect(PlanImages(small, 5800 * MiB, 60).age_frames == 60, "age from KYTY_VRAM_PRESSURE_FRAMES");
	Expect(PlanImages(small, 6200 * MiB, 8).age_frames == 4, "a quarter, at least 4 frames");
	// No budget (the driver reports none): nothing.
	Expect(!PlanImages(0, 5 * GiB, 0).retire, "no budget: no retirement");
}

} // namespace

int main() {
	TestPlanningBudgets();
	TestRetainedImageBudgets();
	TestMarks();
	TestPlans();
	if (g_failures != 0) {
		std::printf("%d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("vram_budget: all tests passed\n");
	return 0;
}
