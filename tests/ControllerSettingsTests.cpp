// Exercise the production controller with deterministic host output and time.
#include <SDL3/SDL.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
void Check(bool condition, const char* text) {
	if (!condition) {
		std::fprintf(stderr, "ControllerSettingsTests: %s\n", text);
		std::abort();
	}
}

struct Rumble {
	Uint16 large, small;
	Uint32 duration;
};
Uint64                             now = 1000;
std::vector<Rumble>                rumble;
std::vector<Rumble>                haptics;
std::vector<std::array<Uint8, 32>> effects;
bool                               haptics_handles_rumble = false;
bool                               trigger_send_succeeds = true;
int                                joystick_lock_depth = 0;
} // namespace

namespace Fake {
void LockJoysticks() { ++joystick_lock_depth; }
void UnlockJoysticks() {
	Check(joystick_lock_depth > 0, "joystick lock was not held");
	--joystick_lock_depth;
}
Uint64 GetTicks() {
	return now;
}
SDL_Gamepad* GetGamepadFromID(SDL_JoystickID id) {
	return id == 1 || id == 2 ? reinterpret_cast<SDL_Gamepad*>(static_cast<uintptr_t>(id))
	                          : nullptr;
}
SDL_GamepadType GetGamepadType(SDL_Gamepad*) {
	return SDL_GAMEPAD_TYPE_PS5;
}
bool RumbleGamepad(SDL_Gamepad*, Uint16 large, Uint16 small, Uint32 duration) {
	rumble.push_back({large, small, duration});
	return true;
}
bool SendGamepadEffect(SDL_Gamepad*, const void* data, int size) {
	Check(size == 32, "unexpected DualSense effect size");
	std::array<Uint8, 32> effect {};
	std::memcpy(effect.data(), data, effect.size());
	effects.push_back(effect);
	return trigger_send_succeeds;
}
bool SetGamepadLED(SDL_Gamepad*, Uint8, Uint8, Uint8) {
	return true;
}
bool GamepadHasSensor(SDL_Gamepad*, SDL_SensorType) {
	return false;
}
bool SetGamepadSensorEnabled(SDL_Gamepad*, SDL_SensorType, bool) {
	return true;
}
void CloseGamepad(SDL_Gamepad*) {}
void Delay(Uint32) {}
} // namespace Fake

#define SDL_GetTicks                Fake::GetTicks
#define SDL_GetGamepadFromID        Fake::GetGamepadFromID
#define SDL_GetGamepadType          Fake::GetGamepadType
#define SDL_RumbleGamepad           Fake::RumbleGamepad
#define SDL_SendGamepadEffect       Fake::SendGamepadEffect
#define SDL_SetGamepadLED           Fake::SetGamepadLED
#define SDL_GamepadHasSensor        Fake::GamepadHasSensor
#define SDL_SetGamepadSensorEnabled Fake::SetGamepadSensorEnabled
#define SDL_CloseGamepad            Fake::CloseGamepad
#define SDL_Delay                   Fake::Delay
#define SDL_LockJoysticks           Fake::LockJoysticks
#define SDL_UnlockJoysticks         Fake::UnlockJoysticks
#include "libs/controller.cpp"
#undef SDL_GetTicks
#undef SDL_GetGamepadFromID
#undef SDL_GetGamepadType
#undef SDL_RumbleGamepad
#undef SDL_SendGamepadEffect
#undef SDL_SetGamepadLED
#undef SDL_GamepadHasSensor
#undef SDL_SetGamepadSensorEnabled
#undef SDL_CloseGamepad
#undef SDL_Delay
#undef SDL_LockJoysticks
#undef SDL_UnlockJoysticks

namespace Libs::Controller::DualSenseHaptics {
bool SetVibration(int, uint8_t large_motor, uint8_t small_motor, uint32_t duration_ms) {
	haptics.push_back({large_motor, small_motor, duration_ms});
	return haptics_handles_rumble;
}
void Shutdown() {}
} // namespace Libs::Controller::DualSenseHaptics

namespace Libs::LibKernel {
uint64_t KYTY_SYSV_ABI KernelGetProcessTime() {
	return now * 1000;
}
} // namespace Libs::LibKernel

namespace Loader::Timer {
double GetTimeMs() {
	return static_cast<double>(now);
}
} // namespace Loader::Timer

namespace {
using namespace Libs::Controller;

struct Controller {
	Controller() {
		now                    = 1000;
		haptics_handles_rumble = false;
		trigger_send_succeeds = true;
		Initialize();
		Connect(1);
		Check(GetSettingScale(Setting::SpeakerVolume) ==
		              Config::GetControllerSpeakerVolume() / 100.0f &&
		          GetSettingScale(Setting::VibrationIntensity) ==
		              Config::GetControllerVibrationIntensity() / 100.0f &&
		          GetSettingScale(Setting::TriggerEffectIntensity) == 1.0f,
		      "controller initialization retained old settings");
		rumble.clear();
		haptics.clear();
		effects.clear();
	}
	~Controller() { Shutdown(); }
};

DualSenseEffects LastEffect() {
	Check(!effects.empty(), "missing trigger output");
	DualSenseEffects effect {};
	std::memcpy(&effect, effects.back().data(), sizeof(effect));
	return effect;
}

int ZoneStrength(const uint8_t* effect, int zone) {
	const unsigned active = effect[1] | (effect[2] << 8u);
	const uint32_t packed = effect[3] | (effect[4] << 8u) | (effect[5] << 16u) |
	                        (static_cast<uint32_t>(effect[6]) << 24u);
	return (active & (1u << zone)) != 0 ? 1 + ((packed >> (3 * zone)) & 7u) : 0;
}

void SetRumble(uint8_t large, uint8_t small) {
	const PadVibrationParam param {large, small};
	Check(PadSetVibration(1, &param) == 0, "vibration request failed");
}

void TestSettingCycles() {
	Controller controller;
	for (auto setting:
	     {Setting::SpeakerVolume, Setting::VibrationIntensity, Setting::TriggerEffectIntensity}) {
		Check(GetSettingScale(setting) == 1.0f, "initial setting is not strong");
		const std::vector<float> levels = setting == Setting::SpeakerVolume
		                                      ? std::vector<float> {0.0f, 0.02f, 0.12f, 0.42f, 1.0f}
		                                      : std::vector<float> {0.0f, 0.33f, 0.66f, 1.0f};
		for (int cycle = 0; cycle < 2; ++cycle) {
			for (float level: levels) {
				CycleSetting(setting);
				Check(GetSettingScale(setting) == level, "setting did not cycle or wrap");
			}
		}
	}
	CycleSetting(Setting::SpeakerVolume);
	CycleSetting(Setting::VibrationIntensity);
	CycleSetting(Setting::TriggerEffectIntensity);
}

void TestGlobalControllerLevels() {
	Config::ConfigOptions options;
	options.controller_speaker_volume      = 50;
	options.controller_vibration_intensity = 25;
	Config::Load(options);
	{
		Controller controller;
		Check(GetSettingScale(Setting::SpeakerVolume) == 0.5f &&
		          GetSettingScale(Setting::VibrationIntensity) == 0.25f &&
		          GetSettingScale(Setting::TriggerEffectIntensity) == 1.0f,
		      "global controller levels did not scale their outputs independently");
		SetRumble(200, 100);
		Check(rumble.back().large == 50 * 257 && rumble.back().small == 25 * 257,
		      "global vibration level did not scale rumble");
		CycleSetting(Setting::SpeakerVolume);
		CycleSetting(Setting::VibrationIntensity);
		Check(GetSettingScale(Setting::SpeakerVolume) == 0.0f &&
		          GetSettingScale(Setting::VibrationIntensity) == 0.0f,
		      "cycle hotkeys did not mute globally scaled outputs");
		CycleSetting(Setting::SpeakerVolume);
		CycleSetting(Setting::VibrationIntensity);
		Check(GetSettingScale(Setting::SpeakerVolume) == 0.01f &&
		          GetSettingScale(Setting::VibrationIntensity) == 0.0825f,
		      "cycle hotkeys did not multiply the global controller levels");
		Check(rumble.back().large == 17 * 257 && rumble.back().small == 8 * 257,
		      "cycling vibration did not apply the combined intensity to cached rumble");
	}
	options.controller_speaker_volume      = 0;
	options.controller_vibration_intensity = 0;
	Config::Load(options);
	{
		Controller controller;
		Check(GetSettingScale(Setting::SpeakerVolume) == 0.0f &&
		          GetSettingScale(Setting::VibrationIntensity) == 0.0f,
		      "zero global controller levels did not mute outputs");
		SetRumble(200, 100);
		Check(rumble.back().large == 0 && rumble.back().small == 0,
		      "zero global vibration level did not mute motors");
	}
	Config::Load(Config::ConfigOptions {});
}

void TestVibrationLifetime() {
	Controller controller;
	SetRumble(255, 1);
	Check(rumble.size() == 1 && rumble.back().large == 65535 && rumble.back().small == 257 &&
	          rumble.back().duration == 65535,
	      "full vibration changed");
	now += 100;
	CycleSetting(Setting::VibrationIntensity);
	Check(rumble.back().large == 0 && rumble.back().small == 0, "muting did not stop vibration");
	now += 100;
	CycleSetting(Setting::VibrationIntensity);
	Check(rumble.back().large == 84 * 257 && rumble.back().small == 257 &&
	          rumble.back().duration == 65335,
	      "changing intensity lost a small motor or extended the vibration deadline");
	Check(effects.empty(), "vibration intensity resent triggers");
	now              = 1000 + 65535;
	const auto calls = rumble.size();
	CycleSetting(Setting::VibrationIntensity);
	CycleSetting(Setting::VibrationIntensity);
	Check(rumble.size() == calls || (rumble.back().large == 0 && rumble.back().small == 0),
	      "expired vibration was resurrected");
	SetRumble(0, 0);
	const auto stopped = rumble.size();
	CycleSetting(Setting::VibrationIntensity);
	CycleSetting(Setting::VibrationIntensity);
	Check(rumble.size() == stopped || (rumble.back().large == 0 && rumble.back().small == 0),
	      "explicitly stopped vibration was resurrected");
}

void TestMaskedTriggersAndValidation() {
	Controller            controller;
	PadTriggerEffectParam param {};
	param.trigger_mask       = 3;
	param.command[0].mode    = 1;
	param.command[0].data[0] = 2;
	param.command[0].data[1] = 8;
	param.command[1].mode    = 2;
	param.command[1].data[0] = 2;
	param.command[1].data[1] = 7;
	param.command[1].data[2] = 6;
	Check(PadSetTriggerEffect(1, &param) == 0, "initial trigger request failed");
	CycleSetting(Setting::TriggerEffectIntensity);
	Check(LastEffect().left_trigger[0] == 5 && LastEffect().right_trigger[0] == 5,
	      "muting did not disable both triggers");
	for (uint32_t mode = 1; mode <= 7; ++mode) {
		auto invalid            = param;
		invalid.command[1]      = {};
		invalid.command[1].mode = mode;
		std::memset(invalid.command[1].data, 255, sizeof(invalid.command[1].data));
		const auto calls = effects.size();
		Check(PadSetTriggerEffect(1, &invalid) == PAD_ERROR_INVALID_ARG && effects.size() == calls,
		      "muting bypassed trigger validation or sent a partial invalid request");
	}
	param.trigger_mask = 4;
	Check(PadSetTriggerEffect(1, &param) == PAD_ERROR_INVALID_ARG, "invalid trigger mask accepted");
	param.trigger_mask       = 1;
	param.command[0].data[1] = 6;
	Check(PadSetTriggerEffect(1, &param) == 0 && LastEffect().enable_bits == 8,
	      "left-only update touched the right trigger");
	CycleSetting(Setting::TriggerEffectIntensity);
	auto effect = LastEffect();
	Check(effect.enable_bits == 12 && effect.left_trigger[0] == 0x21 &&
	          ZoneStrength(effect.left_trigger, 1) == 0 &&
	          ZoneStrength(effect.left_trigger, 2) == 2 && effect.right_trigger[0] == 0x25 &&
	          effect.right_trigger[1] == 0x84 && effect.right_trigger[3] == 1,
	      "masked updates lost a cached trigger or failed to scale its strength");
	CycleSetting(Setting::TriggerEffectIntensity);
	CycleSetting(Setting::TriggerEffectIntensity);
	effect = LastEffect();
	Check(ZoneStrength(effect.left_trigger, 2) == 6 && effect.right_trigger[3] == 5,
	      "restoring strong intensity did not recover original trigger strengths");
	Check(rumble.empty() && haptics.empty(), "trigger intensity resent vibration");
}

void TestTriggerSendCacheAndRetry() {
	Controller controller;
	PadTriggerEffectParam param {};
	param.trigger_mask = 3;
	for (auto& command: param.command) {
		command.mode = 1;
		command.data[0] = 2;
		command.data[1] = 6;
	}
	Check(PadSetTriggerEffect(1, &param) == 0 && effects.size() == 1,
	      "initial trigger effect did not reach the host");
	Check(PadSetTriggerEffect(1, &param) == 0 && effects.size() == 1,
	      "unchanged delivered trigger effects were resent");
	param.trigger_mask = 1;
	param.command[0].data[1] = 8;
	trigger_send_succeeds = false;
	Check(PadSetTriggerEffect(1, &param) == 0 && effects.size() == 2,
	      "failed host send was not attempted");
	auto right = param;
	right.trigger_mask = 2;
	right.command[1].data[1] = 7;
	trigger_send_succeeds = true;
	Check(PadSetTriggerEffect(1, &right) == 0 && effects.size() == 3,
	      "independent right trigger update failed");
	Check(PadSetTriggerEffect(1, &param) == 0 && effects.size() == 4,
	      "right trigger success incorrectly suppressed a failed left-trigger retry");
	Check(PadSetTriggerEffect(1, &param) == 0 && effects.size() == 4,
	      "successful left trigger retry was not cached");
	CycleSetting(Setting::TriggerEffectIntensity);
	Check(effects.size() == 5 && LastEffect().left_trigger[0] == 5 &&
	          LastEffect().right_trigger[0] == 5,
	      "trigger cache prevented an intensity change from disabling both effects");
	Check(joystick_lock_depth == 0, "trigger submission leaked a joystick lock");
}

void TestTriggerEffectStatesAndTravel() {
	Controller controller;
	int32_t state[2] {-1, -1};
	Check(PadGetTriggerEffectState(2, state) == PAD_ERROR_INVALID_HANDLE,
	      "trigger state accepted an invalid handle");
	Check(PadGetTriggerEffectState(1, nullptr) == -2137653243,
	      "trigger state accepted a null output pointer");
	Check(PadGetTriggerEffectState(1, state) == 0 && state[0] == 0 && state[1] == 0,
	      "initial trigger state was not off");
	PadTriggerEffectParam param {};
	param.trigger_mask = 3;
	param.command[0].mode = 1;
	param.command[0].data[0] = 4;
	param.command[0].data[1] = 8;
	param.command[1].mode = 2;
	param.command[1].data[0] = 2;
	param.command[1].data[1] = 7;
	param.command[1].data[2] = 6;
	Check(PadSetTriggerEffect(1, &param) == 0, "feedback/weapon setup failed");
	const auto check_state = [&](int left, int right, int expected_left, int expected_right) {
		SetAxis(1, Axis::TriggerLeft, left);
		SetAxis(1, Axis::TriggerRight, right);
		Check(PadGetTriggerEffectState(1, state) == 0 && state[0] == expected_left &&
		          state[1] == expected_right, "trigger state did not follow command and travel");
	};
	check_state(0, 0, 1, 3);
	check_state(102, 52, 1, 4);
	check_state(103, 178, 2, 4);
	check_state(255, 180, 2, 5);
	param.trigger_mask = 1;
	param.command[0] = {};
	param.command[0].mode = 3;
	param.command[0].data[0] = 0;
	param.command[0].data[1] = 4;
	param.command[0].data[2] = 20;
	Check(PadSetTriggerEffect(1, &param) == 0, "vibration setup failed");
	check_state(0, 255, 6, 5);
	check_state(7, 0, 6, 3);
	check_state(8, 0, 7, 3);
	CycleSetting(Setting::TriggerEffectIntensity);
	Check(PadGetTriggerEffectState(1, state) == 0 && state[0] == 7,
	      "muting physical resistance disabled a guest gameplay action");
	auto invalid = param;
	invalid.command[0].data[1] = 255;
	Check(PadSetTriggerEffect(1, &invalid) == PAD_ERROR_INVALID_ARG &&
	          PadGetTriggerEffectState(1, state) == 0 && state[0] == 7,
	      "rejected trigger command changed the accepted guest state");
	param.command[0] = {};
	param.command[0].mode = 4;
	param.command[0].data[4] = 6;
	Check(PadSetTriggerEffect(1, &param) == 0, "multi-position feedback setup failed");
	check_state(102, 0, 1, 3);
	check_state(103, 0, 2, 3);
	check_state(255, 0, 1, 3);
	param.command[0] = {};
	param.command[0].mode = 5;
	param.command[0].data[0] = 3;
	param.command[0].data[1] = 7;
	param.command[0].data[2] = 2;
	param.command[0].data[3] = 6;
	Check(PadSetTriggerEffect(1, &param) == 0, "slope feedback setup failed");
	check_state(76, 0, 1, 3);
	check_state(77, 0, 2, 3);
	param.command[0] = {};
	param.command[0].mode = 6;
	param.command[0].data[0] = 30;
	param.command[0].data[1] = 5;
	param.command[0].data[10] = 5;
	Check(PadSetTriggerEffect(1, &param) == 0, "multi-position vibration setup failed");
	check_state(0, 0, 6, 3);
	check_state(8, 0, 7, 3);
	check_state(128, 0, 6, 3);
	check_state(255, 0, 7, 3);
	param.command[0] = {};
	param.command[0].mode = 1;
	param.command[0].data[1] = 5;
	Check(PadSetTriggerEffect(1, &param) == 0, "digital feedback setup failed");
	check_state(0, 0, 1, 3);
	SetButton(1, PAD_BUTTON_L2, true);
	Check(PadGetTriggerEffectState(1, state) == 0 && state[0] == 2,
	      "a digital L2 press did not activate feedback");
	PadData data {};
	Check(PadReadState(1, &data) == 0 && data.analog_buttons_l2 == 255 &&
	          data.analog_buttons_r2 == 0, "digital L2 did not supply full analog travel independently");
	SetButton(1, PAD_BUTTON_L2, false);
	Check(PadGetTriggerEffectState(1, state) == 0 && state[0] == 1,
	      "digital L2 release retained a pressed effect");
	SetAxis(1, Axis::TriggerLeft, 8);
	Check(PadReadState(1, &data) == 0 && data.analog_buttons_l2 == 8,
	      "partial analog L2 travel was promoted to full travel");
	param.command[0] = {};
	Check(PadSetTriggerEffect(1, &param) == 0 && PadGetTriggerEffectState(1, state) == 0 &&
	          state[0] == 0 && state[1] == 3, "off command affected the other trigger state");
	Disconnect(1);
	Check(PadGetTriggerEffectState(1, state) == 0 && state[0] == 0 && state[1] == 0,
	      "disconnected pad retained trigger effects");
}

void TestIndependentOutputsAndPadSwitch() {
	Controller controller;
	haptics_handles_rumble = true;
	SetRumble(200, 100);
	now += 100;
	CycleSetting(Setting::VibrationIntensity);
	CycleSetting(Setting::VibrationIntensity);
	Check(rumble.empty() && haptics.back().large == 66 && haptics.back().small == 33 &&
	          haptics.back().duration == 65435,
	      "haptics rumble did not receive scaled motors and remaining duration");
	PadTriggerEffectParam param {};
	param.trigger_mask       = 1;
	param.command[0].mode    = 1;
	param.command[0].data[1] = 8;
	Check(PadSetTriggerEffect(1, &param) == 0, "trigger request failed");
	const auto vibration_calls = haptics.size();
	const auto trigger_calls   = effects.size();
	CycleSetting(Setting::SpeakerVolume);
	Check(haptics.size() == vibration_calls && effects.size() == trigger_calls,
	      "speaker volume resent controller effects");
	Connect(2);
	Disconnect(1);
	Check(GetActiveControllerId() == 2, "active controller did not change");
	haptics.clear();
	effects.clear();
	CycleSetting(Setting::VibrationIntensity);
	CycleSetting(Setting::TriggerEffectIntensity);
	CycleSetting(Setting::TriggerEffectIntensity);
	Check(effects.empty(), "previous controller's trigger effects leaked to new pad");
	for (const auto& request: haptics) {
		Check(request.large == 0 && request.small == 0,
		      "previous controller's vibration leaked to new pad");
	}
	SetRumble(200, 100);
	Check(PadSetTriggerEffect(1, &param) == 0, "replacement pad trigger request failed");
	EmergencyShutdown();
	Check(GetActiveControllerId() == -1, "released controller remained active");
	haptics.clear();
	effects.clear();
	CycleSetting(Setting::VibrationIntensity);
	CycleSetting(Setting::TriggerEffectIntensity);
	Check(effects.empty(), "released controller received a trigger request");
	for (const auto& request: haptics) {
		Check(request.large == 0 && request.small == 0,
		      "released controller retained a cached vibration");
	}
}
} // namespace

int main() {
	Config::Initialize();
	TestSettingCycles();
	TestGlobalControllerLevels();
	// Each following test initializes another controller and requires strong defaults.
	TestVibrationLifetime();
	TestMaskedTriggersAndValidation();
	TestTriggerSendCacheAndRetry();
	TestTriggerEffectStatesAndTravel();
	TestIndependentOutputsAndPadSwitch();
	Config::Shutdown();
	return 0;
}
