#ifndef KYTY_COMMON_DIRECT_STORAGE_H_
#define KYTY_COMMON_DIRECT_STORAGE_H_

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

namespace Common {

// Byte-preserving transport for native, read-only files. Archive members and
// memory files continue to use their existing readers. No codec is inferred.
class DirectStorageReader {
public:
	static constexpr uint32_t ChunkSize = 256 * 1024;

	// native_file is a borrowed Windows HANDLE. Verify that DirectStorage opened
	// the same file before attaching; the caller keeps the handle alive.
	static std::unique_ptr<DirectStorageReader> Open(const std::filesystem::path& path,
	                                               void* native_file);
	~DirectStorageReader();
	DirectStorageReader(const DirectStorageReader&) = delete;
	DirectStorageReader& operator=(const DirectStorageReader&) = delete;

	// Returns the byte count, including zero at EOF. nullopt requests ordinary
	// I/O from the original offset. All queued writes are drained before return,
	// including on failure. Reads never change the native file's position.
	std::optional<uint32_t> ReadAt(void* destination, uint32_t size, uint64_t offset);

private:
	struct Private;
	explicit DirectStorageReader(std::unique_ptr<Private> state);
	std::unique_ptr<Private> m_p;
};

} // namespace Common

#endif
