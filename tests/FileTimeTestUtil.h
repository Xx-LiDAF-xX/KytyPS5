#ifndef KYTY_TESTS_FILE_TIME_TEST_UTIL_H_
#define KYTY_TESTS_FILE_TIME_TEST_UTIL_H_

#include <chrono>
#include <cstdint>
#include <filesystem>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// Sets a host file's write time to `unix_ns` nanoseconds since the Unix epoch.
inline void SetUnixWriteTime(const std::filesystem::path& path, int64_t unix_ns) {
#ifdef _WIN32
	ULARGE_INTEGER ull {};
	ull.QuadPart = static_cast<ULONGLONG>(unix_ns / 100) + 116444736000000000ULL;
	FILETIME ft {};
	ft.dwLowDateTime  = ull.LowPart;
	ft.dwHighDateTime = ull.HighPart;
	HANDLE h = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
	                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
	                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h != INVALID_HANDLE_VALUE) {
		SetFileTime(h, nullptr, nullptr, &ft);
		CloseHandle(h);
	}
#else
	const std::chrono::sys_time<std::chrono::nanoseconds> time {std::chrono::nanoseconds(unix_ns)};
	const auto file_time = std::chrono::file_clock::from_sys(time);
	std::filesystem::last_write_time(
	    path, std::chrono::floor<std::filesystem::file_time_type::duration>(file_time));
#endif
}

#endif // KYTY_TESTS_FILE_TIME_TEST_UTIL_H_
