#include "common/directStorage.h"
#include "common/logging/log.h"

#if defined(_WIN32) && defined(KYTY_ENABLE_DIRECTSTORAGE)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dstorage.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <utility>

namespace Common {
namespace {

using Microsoft::WRL::ComPtr;
constexpr uint32_t SlotCount = 8;
constexpr size_t RingSize = SlotCount * DirectStorageReader::ChunkSize;

struct Batch {
	std::array<uint32_t, SlotCount> slots {};
	uint32_t count = 0;
};

class StorageQueue {
public:
	StorageQueue() {
		wchar_t enabled[2] {};
		if (GetEnvironmentVariableW(L"KYTY_DIRECTSTORAGE", enabled, 2) != 1 || enabled[0] != L'1') {
			return;
		}
		m_module = LoadLibraryExW(L"dstorage.dll", nullptr,
		                         LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
		if (m_module == nullptr) {
			Unavailable("dstorage.dll could not be loaded");
			return;
		}
		using GetFactory = HRESULT (WINAPI*)(REFIID, void**);
		const auto get_factory = reinterpret_cast<GetFactory>(GetProcAddress(m_module, "DStorageGetFactory"));
		if (get_factory == nullptr || FAILED(get_factory(IID_PPV_ARGS(&factory)))) {
			Unavailable("factory initialization failed");
			return;
		}
		m_ring = static_cast<uint8_t*>(VirtualAlloc(nullptr, RingSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
		if (m_ring == nullptr || !VirtualLock(m_ring, RingSize)) {
			Unavailable("the 2 MiB ring could not be page-locked");
			return;
		}
		m_locked = true;
		DSTORAGE_QUEUE_DESC description {};
		description.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
		description.Capacity = DSTORAGE_MIN_QUEUE_CAPACITY;
		description.Priority = DSTORAGE_PRIORITY_NORMAL;
		description.Name = "KytyPS5 raw asset reads";
		description.Device = nullptr;
		if (FAILED(factory->CreateQueue(&description, IID_PPV_ARGS(&queue))) ||
		    FAILED(factory->CreateStatusArray(SlotCount, "KytyPS5 ring slots", IID_PPV_ARGS(&status)))) {
			Unavailable("queue initialization failed");
			return;
		}
		for (auto& event : m_events) {
			event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			if (event == nullptr) {
				Unavailable("completion event allocation failed");
				return;
			}
		}
		available = true;
		Log::Printf("DirectStorage: raw reads enabled (2 MiB page-locked ring).\n");
	}

	~StorageQueue() {
		// Readers retain shared ownership of this queue through their last read.
		if (queue) {
			queue->Close();
		}
		queue.Reset();
		status.Reset();
		factory.Reset();
		for (const auto event : m_events) {
			if (event != nullptr) {
				CloseHandle(event);
			}
		}
		if (m_locked) {
			VirtualUnlock(m_ring, RingSize);
		}
		if (m_ring != nullptr) {
			VirtualFree(m_ring, 0, MEM_RELEASE);
		}
		if (m_module != nullptr) {
			FreeLibrary(m_module);
		}
	}

	Batch Acquire(uint32_t maximum) {
		std::unique_lock lock(m_ring_mutex);
		m_free.wait(lock, [this] {
			return std::any_of(m_busy.begin(), m_busy.end(), [](bool busy) { return !busy; });
		});
		Batch batch;
		for (uint32_t i = 0; i < SlotCount && batch.count < maximum; ++i) {
			const auto slot = (m_next + i) % SlotCount;
			if (!m_busy[slot]) {
				m_busy[slot] = true;
				batch.slots[batch.count++] = slot;
			}
		}
		m_next = (batch.slots[batch.count - 1] + 1) % SlotCount;
		return batch;
	}

	void Release(uint32_t slot) {
		{
			std::scoped_lock lock(m_ring_mutex);
			m_busy[slot] = false;
		}
		m_free.notify_all();
	}

	uint8_t* Data(uint32_t slot) const { return m_ring + slot * DirectStorageReader::ChunkSize; }
	HANDLE Event(uint32_t slot) const { return m_events[slot]; }

	bool available = false;
	ComPtr<IDStorageFactory> factory;
	ComPtr<IDStorageQueue1> queue;
	ComPtr<IDStorageStatusArray> status;
	std::mutex submission_mutex;

private:
	static void Unavailable(const char* reason) {
		Log::Printf("DirectStorage: %s; using ordinary file I/O.\n", reason);
	}
	HMODULE m_module = nullptr;
	uint8_t* m_ring = nullptr;
	bool m_locked = false;
	std::array<HANDLE, SlotCount> m_events {};
	std::array<bool, SlotCount> m_busy {};
	uint32_t m_next = 0;
	std::mutex m_ring_mutex;
	std::condition_variable m_free;
};

std::shared_ptr<StorageQueue> GetQueue() {
	static const auto queue = std::make_shared<StorageQueue>();
	return queue;
}

bool SameFile(const BY_HANDLE_FILE_INFORMATION& a, const BY_HANDLE_FILE_INFORMATION& b) {
	return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber &&
	       a.nFileIndexHigh == b.nFileIndexHigh && a.nFileIndexLow == b.nFileIndexLow;
}

} // namespace

struct DirectStorageReader::Private {
	std::shared_ptr<StorageQueue> storage;
	ComPtr<IDStorageFile> file;
	HANDLE native_file = nullptr;
	~Private() {
		if (file) {
			file->Close();
		}
	}
};

DirectStorageReader::DirectStorageReader(std::unique_ptr<Private> state): m_p(std::move(state)) {}
DirectStorageReader::~DirectStorageReader() = default;

std::unique_ptr<DirectStorageReader> DirectStorageReader::Open(const std::filesystem::path& path,
                                                            void* native_file) {
	auto storage = GetQueue();
	if (!storage->available) {
		return nullptr;
	}
	auto state = std::make_unique<Private>();
	state->storage = std::move(storage);
	state->native_file = native_file;
	BY_HANDLE_FILE_INFORMATION native_info {}, storage_info {};
	if (!GetFileInformationByHandle(native_file, &native_info) ||
	    (native_info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
	    FAILED(state->storage->factory->OpenFile(path.c_str(), IID_PPV_ARGS(&state->file))) ||
	    FAILED(state->file->GetFileInformation(&storage_info)) || !SameFile(native_info, storage_info)) {
		return nullptr;
	}
	return std::unique_ptr<DirectStorageReader>(new DirectStorageReader(std::move(state)));
}

std::optional<uint32_t> DirectStorageReader::ReadAt(void* destination, uint32_t size, uint64_t offset) {
	if (size == 0) {
		return 0;
	}
	if (destination == nullptr) {
		return std::nullopt;
	}
	LARGE_INTEGER length {};
	if (!GetFileSizeEx(m_p->native_file, &length) || length.QuadPart < 0) {
		return std::nullopt;
	}
	const auto file_size = static_cast<uint64_t>(length.QuadPart);
	if (offset >= file_size) {
		return 0;
	}
	const auto readable = static_cast<uint32_t>(std::min<uint64_t>(size, file_size - offset));
	auto& storage = *m_p->storage;
	uint32_t copied = 0;
	while (copied < readable) {
		const auto remaining = readable - copied;
		const auto chunks = (static_cast<uint64_t>(remaining) + ChunkSize - 1) / ChunkSize;
		const auto batch = storage.Acquire(static_cast<uint32_t>(std::min<uint64_t>(SlotCount, chunks)));
		std::array<uint32_t, SlotCount> sizes {};
		uint32_t queued = 0;
		{
			// Keep each request/status/event triplet together. Status then reports
			// only that request's errors, even with concurrent file readers.
			std::scoped_lock lock(storage.submission_mutex);
			for (uint32_t i = 0; i < batch.count; ++i) {
				const auto slot = batch.slots[i];
				sizes[i] = std::min(ChunkSize, remaining - queued);
				DSTORAGE_REQUEST request {};
				request.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
				request.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
				request.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_NONE;
				request.Source.File.Source = m_p->file.Get();
				request.Source.File.Offset = offset + copied + queued;
				request.Source.File.Size = sizes[i];
				request.Destination.Memory.Buffer = storage.Data(slot);
				request.Destination.Memory.Size = sizes[i];
				request.UncompressedSize = sizes[i];
				storage.queue->EnqueueRequest(&request);
				storage.queue->EnqueueStatus(storage.status.Get(), slot);
				storage.queue->EnqueueSetEvent(storage.Event(slot));
				queued += sizes[i];
			}
			storage.queue->Submit();
		}
		bool failed = false;
		for (uint32_t i = 0; i < batch.count; ++i) {
			const auto slot = batch.slots[i];
			// Never free or reuse a slot while an I/O write could still target it.
			// The owned event remains valid for the entire queue lifetime.
			const auto wait = WaitForSingleObject(storage.Event(slot), INFINITE);
			if (wait != WAIT_OBJECT_0) {
				// An unexpected wait failure still requires draining the request.
				while (!storage.status->IsComplete(slot)) {
					Sleep(1);
				}
				failed = true;
			}
			failed = failed || FAILED(storage.status->GetHResult(slot));
			if (!failed) {
				std::memcpy(static_cast<uint8_t*>(destination) + copied, storage.Data(slot), sizes[i]);
			}
			copied += sizes[i];
			storage.Release(slot);
		}
		if (failed) {
			return std::nullopt;
		}
	}
	return copied;
}

} // namespace Common

#else

namespace Common {
struct DirectStorageReader::Private {};
DirectStorageReader::DirectStorageReader(std::unique_ptr<Private> state): m_p(std::move(state)) {}
DirectStorageReader::~DirectStorageReader() = default;
std::unique_ptr<DirectStorageReader> DirectStorageReader::Open(const std::filesystem::path&, void*) {
	return nullptr;
}
std::optional<uint32_t> DirectStorageReader::ReadAt(void*, uint32_t, uint64_t) {
	return std::nullopt;
}
} // namespace Common

#endif
