#include "loader/systemContent.h"

#include "common/assert.h"
#include "common/common.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/singleton.h"
#include "common/threads.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <version>
#include <vector>

namespace Loader {

class Psf {
public:
	Psf() = default;
	virtual ~Psf();

	KYTY_CLASS_NO_COPY(Psf);

	void Open(const std::filesystem::path& file_name);
	void OpenChunkDefs(const std::filesystem::path& file_name);
	void Close();

	bool IsValid();

	bool     GetParamInt(const char* name, int32_t* value);
	bool     GetParamString(const char* name, std::string* value);
	bool     GetParamString(const char* name, char* value, size_t value_size);
	uint64_t GetFlexibleMemorySize();

	void DbgPrint();

private:
	enum class ParamType {
		Text,
		Int,
	};

	struct Param {
		std::string name;
		ParamType   type = ParamType::Text;
		std::string string_value;
		std::string string_utf8;
		int32_t     int_value = 0;
	};

	void   OpenJson(const std::filesystem::path& file_name);
	void   AddParamString(const std::string& name, const std::string& value);
	void   AddParamInt(const std::string& name, int32_t value);
	Param* FindParam(const char* name);

	Common::Mutex      m_mutex;
	bool               m_opened = false;
	std::vector<Param> m_params;
	uint64_t           m_flexible_memory_size = 0;
};

class PlayGo {
public:
	PlayGo() = default;
	virtual ~PlayGo();

	KYTY_CLASS_NO_COPY(PlayGo);

	void Open(const std::filesystem::path& file_name);
	void OpenChunkDefs(const std::filesystem::path& file_name);
	void OpenChunkManifest(const std::filesystem::path& file_name);
	void Close();

	bool IsValid();

	bool GetChunksNum(uint32_t* num);

	void DbgPrint();

private:
	Common::File  m_f;
	Common::Mutex m_mutex;
	bool          m_opened     = false;
	uint16_t      m_chunks_num = 0;
};

struct SystemContent {
	std::filesystem::path psf_path;
	Psf                   psf;
	std::filesystem::path playgo_path;
	PlayGo                playgo;
	std::filesystem::path icon_path;
};

Psf::~Psf() {
	Close();
}

void Psf::Open(const std::filesystem::path& file_name) {
	Common::LockGuard lock(m_mutex);

	Close();

	OpenJson(file_name);
}

void Psf::OpenJson(const std::filesystem::path& file_name) {
	Common::File f;

	f.Open(file_name, Common::File::Mode::Read);

	if (f.IsInvalid()) {
		LOGF("Can't open %s\n", Common::PathToString(file_name).c_str());
		return;
	}

	auto buf = f.ReadWholeBuffer();
	if (buf.empty()) {
		LOGF("invalid json file: %s\n", Common::PathToString(file_name).c_str());
		f.Close();
		return;
	}

	struct LocalizedTitle {
		std::string language;
		std::string title;
	};
	std::string                 default_language;
	std::vector<LocalizedTitle> localized_titles;

	const auto* json_begin = reinterpret_cast<const char*>(buf.data());
	const auto* json_end   = json_begin + buf.size();
	auto        param_json = nlohmann::ordered_json::parse(json_begin, json_end, nullptr, false);
	if (param_json.is_discarded() || !param_json.is_object()) {
		LOGF("invalid json file: %s\n", Common::PathToString(file_name).c_str());
		f.Close();
		return;
	}

	auto add_string_param = [&](const char* json_key, const char* param_key) {
		auto it = param_json.find(json_key);
		if (it != param_json.end() && it->is_string()) {
			AddParamString(param_key, it->get_ref<const std::string&>());
		}
	};

	auto add_int_param = [&](const char* json_key, const char* param_key) {
		auto it = param_json.find(json_key);
		if (it == param_json.end() || !it->is_number_integer()) {
			return;
		}
		int64_t value = 0;
		if (it->is_number_unsigned()) {
			value = static_cast<int64_t>(it->get<uint64_t>());
		} else {
			value = it->get<int64_t>();
		}
		AddParamInt(param_key, static_cast<int32_t>(value));
	};

	add_string_param("titleId", "TITLE_ID");
	add_string_param("contentId", "CONTENT_ID");
	add_string_param("contentVersion", "APP_VER");
	add_string_param("appVersion", "APP_VER");
	AddParamInt("ATTRIBUTE", 0);
	add_int_param("attribute", "ATTRIBUTE");
	add_int_param("attribute3", "ATTRIBUTE3");
	add_int_param("userDefinedParam1", "USER_DEFINED_PARAM_1");
	add_int_param("userDefinedParam2", "USER_DEFINED_PARAM_2");
	add_int_param("userDefinedParam3", "USER_DEFINED_PARAM_3");
	add_int_param("userDefinedParam4", "USER_DEFINED_PARAM_4");

	if (auto kernel = param_json.find("kernel");
	    kernel != param_json.end() && kernel->is_object()) {
		if (auto it = kernel->find("flexibleMemorySize"); it != kernel->end()) {
			m_flexible_memory_size = it->get<uint64_t>();
		}
	}

	auto localized = param_json.find("localizedParameters");
	if (localized != param_json.end() && localized->is_object()) {
		auto default_it = localized->find("defaultLanguage");
		if (default_it != localized->end() && default_it->is_string()) {
			default_language = default_it->get_ref<const std::string&>();
		}
		for (auto it = localized->begin(); it != localized->end(); ++it) {
			if (!it->is_object()) {
				continue;
			}
			auto title_it = it->find("titleName");
			if (title_it != it->end() && title_it->is_string()) {
				localized_titles.push_back(
				    {it.key(), title_it->get_ref<const std::string&>()});
			}
		}
	}

	std::string title;
	if (!default_language.empty()) {
		if (const auto it = std::ranges::find(localized_titles, default_language,
		                                      &LocalizedTitle::language);
		    it != localized_titles.end()) {
			title = it->title;
		}
	}
	if (title.empty()) {
		if (const auto it =
		        std::ranges::find(localized_titles, std::string_view {"en-US"}, &LocalizedTitle::language);
		    it != localized_titles.end()) {
			title = it->title;
		}
	}
	if (title.empty() && !localized_titles.empty()) {
		title = localized_titles.front().title;
	}
	if (!title.empty()) {
		AddParamString("TITLE", title);
	}

	f.Close();
	m_opened = true;
}

void Psf::Close() {
	Common::LockGuard lock(m_mutex);

	m_params.clear();
	m_flexible_memory_size = 0;
	m_opened               = false;
}

bool Psf::IsValid() {
	Common::LockGuard lock(m_mutex);

	return m_opened;
}

void Psf::DbgPrint() {
	Common::LockGuard lock(m_mutex);

	if (!m_opened) {
		return;
	}

	for (const auto& param: m_params) {
		switch (param.type) {
			case ParamType::Text: {
				const auto str = Common::ReplaceChar(param.string_value, '\n', ',');
				LOGF("%s\n", std::format("{}, string = {}", param.name, str).c_str());
				break;
			}
			case ParamType::Int:
				LOGF("%s\n",
				     std::format("{}, int = 0x{:08x}", param.name, static_cast<uint32_t>(param.int_value))
				         .c_str());
				break;
			default: EXIT("unknown type\n");
		}
	}
}

Psf::Param* Psf::FindParam(const char* name) {
	const auto it = std::ranges::find(m_params, std::string_view {name}, &Param::name);
	return it != m_params.end() ? &*it : nullptr;
}

void Psf::AddParamString(const std::string& name, const std::string& value) {
	auto* param = FindParam(name.c_str());
	if (param == nullptr) {
		m_params.push_back({});
		param       = &m_params.back();
		param->name = name;
	}

	param->type         = ParamType::Text;
	param->string_utf8  = value;
	param->string_value = value;
}

void Psf::AddParamInt(const std::string& name, int32_t value) {
	auto* param = FindParam(name.c_str());
	if (param == nullptr) {
		m_params.push_back({});
		param       = &m_params.back();
		param->name = name;
	}

	param->type      = ParamType::Int;
	param->int_value = value;
}

bool Psf::GetParamInt(const char* name, int32_t* value) {
	Common::LockGuard lock(m_mutex);

	if (!m_opened) {
		return false;
	}
	if (auto* param = FindParam(name); param != nullptr && param->type == ParamType::Int) {
		*value = param->int_value;
		return true;
	}

	return false;
}

bool Psf::GetParamString(const char* name, std::string* value) {
	Common::LockGuard lock(m_mutex);

	if (!m_opened) {
		return false;
	}
	if (auto* param = FindParam(name); param != nullptr && param->type == ParamType::Text) {
		*value = param->string_value;
		return true;
	}

	return false;
}

bool Psf::GetParamString(const char* name, char* value, size_t value_size) {
	Common::LockGuard lock(m_mutex);

	if (!m_opened) {
		return false;
	}
	if (auto* param = FindParam(name); param != nullptr && param->type == ParamType::Text &&
	                                   param->string_utf8.size() + 1 <= value_size) {
		std::memcpy(value, param->string_utf8.c_str(), param->string_utf8.size() + 1);
		return true;
	}

	return false;
}

uint64_t Psf::GetFlexibleMemorySize() {
	Common::LockGuard lock(m_mutex);

	return m_flexible_memory_size;
}

PlayGo::~PlayGo() {
	Close();
}

void PlayGo::Open(const std::filesystem::path& file_name) {
	Common::LockGuard lock(m_mutex);

	Close();

	m_f.Open(file_name, Common::File::Mode::Read);

	if (m_f.IsInvalid()) {
		LOGF("Can't open %s\n", Common::PathToString(file_name).c_str());
		return;
	}

	uint32_t magic1 = 0;
	constexpr uint32_t PLGO_MAGIC = 0x6f676c70;
	constexpr uint32_t PLGX_MAGIC = 0x78676c70;

	m_f.Read(&magic1, 4);

	if (magic1 != PLGO_MAGIC && magic1 != PLGX_MAGIC) {
		LOGF("invalid file: magic1 = %08" PRIx32 "\n", magic1);
		return;
	}

	m_f.Seek(10);
	m_f.Read(&m_chunks_num, 2);

	m_opened = true;
}

static bool ParseXmlUintAttr(std::string_view tag, std::string_view attr, uint32_t& value) {
	const auto attr_pos = tag.find(attr);
	if (attr_pos == std::string_view::npos) {
		return false;
	}

	auto pos = attr_pos + attr.size();
	while (pos < tag.size() && (tag[pos] == ' ' || tag[pos] == '\t')) {
		pos++;
	}
	if (pos >= tag.size() || tag[pos] != '=') {
		return false;
	}
	pos++;
	while (pos < tag.size() && (tag[pos] == ' ' || tag[pos] == '\t')) {
		pos++;
	}
	if (pos >= tag.size() || (tag[pos] != '"' && tag[pos] != '\'')) {
		return false;
	}

	const char quote = tag[pos++];
	const auto start = pos;
	while (pos < tag.size() && tag[pos] >= '0' && tag[pos] <= '9') {
		pos++;
	}
	if (start == pos || pos >= tag.size() || tag[pos] != quote) {
		return false;
	}

	uint32_t parsed      = 0;
	const auto [ptr, ec] = std::from_chars(tag.data() + start, tag.data() + pos, parsed);
	if (ec != std::errc {} || ptr != tag.data() + pos) {
		return false;
	}

	value = parsed;
	return true;
}

void PlayGo::OpenChunkDefs(const std::filesystem::path& file_name) {
	Common::LockGuard lock(m_mutex);

	if (m_opened) {
		return;
	}

	Common::File f;
	f.Open(file_name, Common::File::Mode::Read);

	if (f.IsInvalid()) {
		LOGF("Can't open %s\n", Common::PathToString(file_name).c_str());
		return;
	}

	auto buf = f.ReadWholeBuffer();
	f.Close();

	if (buf.empty()) {
		LOGF("invalid file: %s\n", Common::PathToString(file_name).c_str());
		return;
	}

	const std::string_view xml(reinterpret_cast<const char*>(buf.data()), buf.size());

	uint32_t max_chunk_id = 0;
	bool     found        = false;

	if (const auto chunks_pos = xml.find("<chunks"); chunks_pos != std::string_view::npos) {
		const auto tag_end = xml.find('>', chunks_pos);
		if (tag_end != std::string_view::npos) {
			uint32_t default_chunk = 0;
			if (ParseXmlUintAttr(xml.substr(chunks_pos, tag_end - chunks_pos), "default_chunk",
			                     default_chunk)) {
				max_chunk_id = std::max(max_chunk_id, default_chunk);
				found        = true;
			}
		}
	}

	size_t pos = 0;
	while ((pos = xml.find("<chunk", pos)) != std::string_view::npos) {
		const auto name_end = pos + 6;
		if (name_end >= xml.size() || !(xml[name_end] == ' ' || xml[name_end] == '\t' ||
		                                xml[name_end] == '\r' || xml[name_end] == '\n')) {
			pos = name_end;
			continue;
		}

		const auto tag_end = xml.find('>', name_end);
		if (tag_end == std::string_view::npos) {
			break;
		}

		uint32_t chunk_id = 0;
		if (ParseXmlUintAttr(xml.substr(pos, tag_end - pos), "id", chunk_id)) {
			max_chunk_id = std::max(max_chunk_id, chunk_id);
			found        = true;
		}

		pos = tag_end + 1;
	}

	if (!found || max_chunk_id >= 1000) {
		LOGF("invalid playgo chunk definitions: max_chunk_id=%" PRIu32 ", path=%s\n", max_chunk_id,
		     Common::PathToString(file_name).c_str());
		return;
	}

	m_chunks_num = static_cast<uint16_t>(max_chunk_id + 1);
	m_opened     = true;
}

void PlayGo::OpenChunkManifest(const std::filesystem::path& file_name) {
	Common::LockGuard lock(m_mutex);

	if (m_opened) {
		return;
	}

	Common::File f;
	f.Open(file_name, Common::File::Mode::Read);

	if (f.IsInvalid()) {
		LOGF("Can't open %s\n", Common::PathToString(file_name).c_str());
		return;
	}

	auto buf = f.ReadWholeBuffer();
	f.Close();

	constexpr std::array<std::byte, 4> id_field {
	    std::byte {0x08}, std::byte {'i'}, std::byte {'d'}, std::byte {0x00}};
	std::array<bool, 1000>          chunks {};
	uint32_t                        max_chunk_id = 0;
	bool                            found        = false;
	const std::span<const std::byte> bytes {buf};

	for (size_t pos = 0; pos + id_field.size() + sizeof(uint32_t) <= bytes.size(); pos++) {
		if (!std::ranges::equal(bytes.subspan(pos, id_field.size()), id_field)) {
			continue;
		}

		std::array<std::byte, 4> raw_id {};
		std::ranges::copy(bytes.subspan(pos + id_field.size(), raw_id.size()), raw_id.begin());
		const auto chunk_id = std::bit_cast<uint32_t>(raw_id);
		if (chunk_id < chunks.size()) {
			chunks[chunk_id] = true;
			max_chunk_id     = std::max(max_chunk_id, chunk_id);
			found            = true;
		}
	}

	if (!found ||
	    !std::ranges::all_of(std::span {chunks}.first(max_chunk_id + 1), std::identity {})) {
		LOGF("invalid chunk manifest: path=%s\n", Common::PathToString(file_name).c_str());
		return;
	}

	m_chunks_num = static_cast<uint16_t>(max_chunk_id + 1);
	m_opened     = true;
	LOGF("PlayGo: chunks num = %" PRIu16 " (from %s)\n", m_chunks_num,
	     Common::PathToString(file_name).c_str());
}

void PlayGo::Close() {
	Common::LockGuard lock(m_mutex);

	if (!m_f.IsInvalid()) {
		m_f.Close();
	}
	m_opened     = false;
	m_chunks_num = 0;
}

bool PlayGo::IsValid() {
	Common::LockGuard lock(m_mutex);

	return m_opened;
}

bool PlayGo::GetChunksNum(uint32_t* num) {
	Common::LockGuard lock(m_mutex);

	if (m_opened) {
		*num = m_chunks_num;
		return true;
	}
	return false;
}

void PlayGo::DbgPrint() {
	Common::LockGuard lock(m_mutex);

	if (!m_opened) {
		return;
	}
	LOGF("PlayGo: chunks num = %" PRIu16 "\n", m_chunks_num);
}

void SystemContentLoadParamSfo(const std::filesystem::path& file_name) {
	auto* sc = Common::Singleton<SystemContent>::Instance();

	if (!Common::File::IsFileExisting(file_name)) {
		EXIT("Can't find file: %s\n", Common::PathToString(file_name).c_str());
	}

	sc->psf.Open(file_name);

	if (!sc->psf.IsValid()) {
		EXIT("invalid file: %s\n", Common::PathToString(file_name).c_str());
	}

	sc->psf_path = file_name;
	sc->psf.DbgPrint();

	sc->icon_path = file_name.parent_path() / "icon0.png";

	if (!Common::File::IsFileExisting(sc->icon_path)) {
		sc->icon_path.clear();
	}

	sc->playgo_path = file_name.parent_path() / "playgo-chunk.dat";
	sc->playgo.Open(sc->playgo_path);
	if (!sc->playgo.IsValid()) {
		sc->playgo_path = file_name.parent_path().parent_path() / "playgo-chunkdefs.xml";
		sc->playgo.OpenChunkDefs(sc->playgo_path);
	}
	if (!sc->playgo.IsValid()) {
		sc->playgo_path = file_name.parent_path().parent_path() / "Data" / "chunkmanifest";
		sc->playgo.OpenChunkManifest(sc->playgo_path);
	}

	if (sc->playgo.IsValid()) {
		sc->playgo.DbgPrint();
	}
}

std::optional<int32_t> SystemContentParamSfoGetInt(const char* name) {
	if (name == nullptr) {
		return std::nullopt;
	}

	auto*   sc    = Common::Singleton<SystemContent>::Instance();
	int32_t value = 0;
	if (!sc->psf.GetParamInt(name, &value)) {
		return std::nullopt;
	}

	return value;
}

bool SystemContentParamSfoGetInt(const char* name, int32_t* value) {
	if (value == nullptr) {
		return false;
	}

	const auto opt = SystemContentParamSfoGetInt(name);
	if (!opt.has_value()) {
		return false;
	}

	*value = *opt;
	return true;
}

std::optional<std::string> SystemContentParamSfoGetString(const char* name) {
	if (name == nullptr) {
		return std::nullopt;
	}

	auto*       sc = Common::Singleton<SystemContent>::Instance();
	std::string value;
	if (!sc->psf.GetParamString(name, &value)) {
		return std::nullopt;
	}

	return value;
}

bool SystemContentParamSfoGetString(const char* name, std::string* value) {
	if (value == nullptr) {
		return false;
	}

	auto opt = SystemContentParamSfoGetString(name);
	if (!opt.has_value()) {
		return false;
	}

	*value = std::move(*opt);
	return true;
}

bool SystemContentParamSfoGetString(const char* name, char* value, size_t value_size) {
	if (name == nullptr || value == nullptr) {
		return false;
	}

	auto* sc = Common::Singleton<SystemContent>::Instance();

	return sc->psf.GetParamString(name, value, value_size);
}

std::string SystemContentResolveTitleId() {
	auto extract_from_content_id = []() -> std::optional<std::string> {
		auto content_id = SystemContentParamSfoGetString("CONTENT_ID");
		if (content_id.has_value() && content_id->size() == 36 && (*content_id)[6] == '-' &&
		    (*content_id)[16] == '_') {
			return content_id->substr(7, 9);
		}
		return std::nullopt;
	};

	auto title_id = SystemContentParamSfoGetString("TITLE_ID");
	if (title_id.has_value() && title_id->empty()) {
		title_id.reset();
	}
#if defined(__cpp_lib_optional) && __cpp_lib_optional >= 202110L
	return title_id.or_else(extract_from_content_id).value_or(std::string {});
#else
	if (!title_id.has_value()) {
		title_id = extract_from_content_id();
	}
	return title_id.value_or(std::string {});
#endif
}

uint64_t SystemContentGetFlexibleMemorySize() {
	auto* sc = Common::Singleton<SystemContent>::Instance();

	return sc->psf.GetFlexibleMemorySize();
}

bool SystemContentGetIconPath(std::filesystem::path* path) {
    if (path == nullptr) {
        return false;
    }

    auto* sc = Common::Singleton<SystemContent>::Instance();

    if (sc->icon_path.empty()) {
        return false;
    }

    *path = sc->icon_path;

    return true;
}

bool SystemContentGetChunksNum(uint32_t* num) {
	if (num == nullptr) {
		return false;
	}

	auto* sc = Common::Singleton<SystemContent>::Instance();

	return sc->playgo.GetChunksNum(num);
}

} // namespace Loader
