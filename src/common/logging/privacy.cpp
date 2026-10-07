#include "common/logging/log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <regex>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace Log {
namespace {
bool Word(char c) {
	const auto u = static_cast<unsigned char>(c);
	return std::isalnum(u) != 0 || c == '_';
}

const std::vector<std::string>& PrivateNames() {
	static const auto names = [] {
		std::vector<std::string> result;
		for (const char* key : {"USERNAME", "USER", "COMPUTERNAME", "HOSTNAME"}) {
			if (const char* value = std::getenv(key); value != nullptr && *value != '\0') {
				result.emplace_back(value);
			}
		}
		return result;
	}();
	return names;
}
} // namespace

std::string RedactPrivateInfo(std::string_view text) {
	std::string result;
	result.reserve(text.size());
	for (size_t i = 0; i < text.size();) {
		const bool drive = i + 2 < text.size() &&
		                   std::isalpha(static_cast<unsigned char>(text[i])) != 0 &&
		                   text[i + 1] == ':' && (text[i + 2] == '/' || text[i + 2] == '\\');
		const bool unc = i + 1 < text.size() && text[i] == '\\' && text[i + 1] == '\\';
		const bool boundary = i == 0 || std::isspace(static_cast<unsigned char>(text[i - 1])) != 0 ||
		                      text[i - 1] == '=' || text[i - 1] == '"' || text[i - 1] == '\'' ||
		                      text[i - 1] == '(';
		const bool posix = boundary && text[i] == '/' && i + 1 < text.size() &&
		                   text[i + 1] != ' ' && text[i + 1] != '\n';
		const bool relative = boundary && i + 1 < text.size() && text[i] == '.' &&
		                      (text[i + 1] == '/' || text[i + 1] == '\\' ||
		                       (i + 2 < text.size() && text[i + 1] == '.' &&
		                        (text[i + 2] == '/' || text[i + 2] == '\\')));
		if (drive || unc || posix || relative) {
			// Paths can contain spaces. Conservatively hide through the next hard delimiter.
			i = text.find_first_of("\r\n\t\"'<>|,;)", i);
			if (i == std::string_view::npos) { i = text.size(); }
			result += "[path redacted]";
			continue;
		}
		result += text[i++];
	}
	for (const auto& name : PrivateNames()) {
		for (size_t i = 0; i + name.size() <= result.size();) {
			const bool equal = std::equal(name.begin(), name.end(), result.begin() + i,
			    [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) ==
			                              std::tolower(static_cast<unsigned char>(b)); });
			if (equal && (i == 0 || !Word(result[i - 1])) &&
			    (i + name.size() == result.size() || !Word(result[i + name.size()]))) {
				result.replace(i, name.size(), "[identity redacted]");
				i += sizeof("[identity redacted]") - 1;
			} else { ++i; }
		}
	}
	static const std::regex sensitive(
	    R"((\b(?:username|user_name|hostname|computer_name|password|passwd|token|authorization|cookie|email|serial|mac_address|server_name|field_value|header_value|user_agent|user|name|subject|recipient|ssid|online_id|display_name|value|url|src_url)\s*[:=]\s*)[^\r\n]*)",
	    std::regex::icase | std::regex::optimize);
	static const std::regex network(
	    R"((?:https?|ftp)://[^\s"<>]+|[A-Za-z0-9_.+%-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}|\b(?:(?:25[0-5]|2[0-4][0-9]|1[0-9]{2}|[1-9]?[0-9])\.){3}(?:25[0-5]|2[0-4][0-9]|1[0-9]{2}|[1-9]?[0-9])(?![0-9.])|\b(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}\b)",
	    std::regex::optimize);
	result = std::regex_replace(result, sensitive, "$1[private value redacted]");
	if (result.find_first_of("@.:") != std::string::npos) {
		result = std::regex_replace(result, network, "[network value redacted]");
	}
	return result;
}

std::filesystem::path ResolveOutputPath(const std::filesystem::path& configured) {
	if (!configured.empty() && configured.filename() != "_kyty.txt") {
		return configured;
	}
	const auto now = std::chrono::system_clock::now();
	const auto seconds = std::chrono::system_clock::to_time_t(now);
	std::tm utc {};
#if defined(_WIN32)
	gmtime_s(&utc, &seconds);
	const auto pid = GetCurrentProcessId();
#else
	gmtime_r(&seconds, &utc);
	const auto pid = getpid();
#endif
	char date[32] {};
	std::strftime(date, sizeof(date), "%Y%m%d-%H%M%S", &utc);
	const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
	static std::atomic<uint64_t> sequence {0};
	return configured.parent_path() / fmt::format("_kyty_{}-{:03}Z-{}-{}.txt", date, millis, pid,
	                                            sequence.fetch_add(1, std::memory_order_relaxed));
}
} // namespace Log
