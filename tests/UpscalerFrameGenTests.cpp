#include "common/emulatorConfig.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "UpscalerFrameGenTests: FAILED: %s\n", message);
		std::abort();
	}
}

void TestUpscalerEnumsAndParsing() {
	using namespace Config;
	Check(ParseUpscalerType("off") == UpscalerType::Off, "parse off");
	Check(ParseUpscalerType("fsr") == UpscalerType::Fsr, "parse fsr");
	Check(ParseUpscalerType("DLSS") == UpscalerType::Dlss, "parse DLSS uppercase");
	Check(UpscalerTypeToString(UpscalerType::Fsr) == "FSR", "FSR to string");
	Check(UpscalerTypeToString(UpscalerType::Dlss) == "DLSS", "DLSS to string");

	Check(ParseUpscaleQuality("native") == UpscaleQuality::NativeAA, "parse native");
	Check(ParseUpscaleQuality("quality") == UpscaleQuality::Quality, "parse quality");
	Check(ParseUpscaleQuality("balanced") == UpscaleQuality::Balanced, "parse balanced");
	Check(ParseUpscaleQuality("performance") == UpscaleQuality::Performance, "parse performance");
	Check(ParseUpscaleQuality("ultra_performance") == UpscaleQuality::UltraPerformance, "parse ultra_performance");
}

void TestSharpnessClampingAndStringParsingEdgeCases() {
	using namespace Config;
	Check(ParseUpscalerType("  DlSs ") == UpscalerType::Dlss, "trimmed DLSS");
	Check(ParseUpscalerType("unknown_mode") == UpscalerType::Off, "unknown defaults to Off");
	Check(ParseUpscaleQuality("Ultra-Performance") == UpscaleQuality::UltraPerformance, "hyphenated ultra-performance");
	Check(ParseUpscaleQuality("invalid") == UpscaleQuality::Quality, "invalid quality defaults to Quality");

	SetUpscaleSharpness(250);
	Check(GetUpscaleSharpness() == 100u, "sharpness clamped to 100");
	SetUpscaleSharpness(50);
	Check(GetUpscaleSharpness() == 50u, "sharpness restored to 50");
}

void TestInternalResolutionScaling16By9() {
	using namespace Config;
	SetUpscalerType(UpscalerType::Dlss);
	SetUpscaleQuality(UpscaleQuality::Quality);
	uint32_t w = 0, h = 0;
	GetInternalRenderResolution(3840, 2160, w, h);
	Check(w == 2560 && h == 1440, "4K Quality -> 2560x1440");

	SetUpscaleQuality(UpscaleQuality::Performance);
	GetInternalRenderResolution(3840, 2160, w, h);
	Check(w == 1920 && h == 1080, "4K Performance -> 1920x1080");

	SetUpscaleQuality(UpscaleQuality::UltraPerformance);
	GetInternalRenderResolution(3840, 2160, w, h);
	Check(w == 1280 && h == 720, "4K UltraPerformance -> 1280x720");

	SetUpscalerType(UpscalerType::Off);
	GetInternalRenderResolution(3840, 2160, w, h);
	Check(w == 3840 && h == 2160, "Off -> native 3840x2160");
}

void TestNon16By9UltrawideAnd16By10ResolutionScaling() {
	using namespace Config;
	SetUpscalerType(UpscalerType::Fsr);
	SetUpscaleQuality(UpscaleQuality::Performance); // 2.0x
	uint32_t w = 0, h = 0;
	GetInternalRenderResolution(3440, 1440, w, h);
	Check(w == 1720 && h == 720, "3440x1440 Performance preserves 21:9 -> 1720x720");
	GetInternalRenderResolution(1280, 800, w, h);
	Check(w == 640 && h == 400, "1280x800 Performance preserves 16:10 -> 640x400");
	SetUpscalerType(UpscalerType::Off);
}

void TestZeroAndSmallResolutionEdgeCases() {
	using namespace Config;
	SetUpscalerType(UpscalerType::Dlss);
	SetUpscaleQuality(UpscaleQuality::UltraPerformance);
	uint32_t w = 0, h = 0;
	GetInternalRenderResolution(0, 0, w, h);
	Check(w >= 2 && h >= 2 && (w % 2 == 0) && (h % 2 == 0), "0x0 clamps to >= 2x2 even");
	GetInternalRenderResolution(1, 1, w, h);
	Check(w >= 2 && h >= 2 && (w % 2 == 0) && (h % 2 == 0), "1x1 clamps to >= 2x2 even");
	SetUpscalerType(UpscalerType::Off);
}

void TestHotkeyCycleAndStatusTag() {
	using namespace Config;
	SetUpscalerType(UpscalerType::Off);
	SetUpscaleQuality(UpscaleQuality::Quality);
	SetFrameGenerationEnabled(false);
	Check(FormatUpscalerStatusTag().empty(), "empty tag when both off");

	Check(CycleUpscalerType() == UpscalerType::Fsr, "Off -> Fsr");
	Check(FormatUpscalerStatusTag() == "[FSR Quality]", "FSR Quality tag");

	Check(ToggleFrameGeneration() == true, "FG Off -> On");
	Check(FormatUpscalerStatusTag() == "[FSR Quality | FG]", "FSR Quality + FG tag");

	Check(CycleUpscalerType() == UpscalerType::Dlss, "Fsr -> Dlss");
	Check(FormatUpscalerStatusTag() == "[DLSS Quality | FG]", "DLSS Quality + FG tag");

	Check(CycleUpscalerType() == UpscalerType::Off, "Dlss -> Off");
	Check(FormatUpscalerStatusTag() == "[FG]", "FG only tag");
	SetFrameGenerationEnabled(false);
}

} // namespace

int main() {
	Config::Initialize();
	TestUpscalerEnumsAndParsing();
	TestSharpnessClampingAndStringParsingEdgeCases();
	TestInternalResolutionScaling16By9();
	TestNon16By9UltrawideAnd16By10ResolutionScaling();
	TestZeroAndSmallResolutionEdgeCases();
	TestHotkeyCycleAndStatusTag();
	Config::Shutdown();
	std::printf("UpscalerFrameGenTests: all checks passed\n");
	return 0;
}
