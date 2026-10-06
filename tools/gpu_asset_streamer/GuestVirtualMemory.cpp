#include "GuestVirtualMemory.h"
#include <cstring>
#include <limits>
#include <stdexcept>
#include <system_error>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__) || (defined(__APPLE__) && defined(__MACH__))
#include <sys/mman.h>
#include <unistd.h>
#include <cerrno>
#else
#error Unsupported operating system
#endif

static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
              "Page counters require lock-free uint32_t atomics");

GuestVirtualMemory::GuestVirtualMemory(std::size_t bytes) : size_(bytes) {
#if defined(_WIN32)
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    pageSize_ = info.dwPageSize;
#else
    const auto size = sysconf(_SC_PAGESIZE);
    if (size <= 0) throw std::runtime_error("Cannot query native page size");
    pageSize_ = static_cast<std::size_t>(size);
#endif
    if (bytes == 0 || bytes > std::numeric_limits<std::size_t>::max() - (pageSize_ - 1)) {
        throw std::invalid_argument("Invalid guest mapping size");
    }
    mappedSize_ = ((bytes + pageSize_ - 1) / pageSize_) * pageSize_;
    pageCount_ = mappedSize_ / pageSize_;
    counters_ = std::make_unique<Counters[]>(pageCount_);
#if defined(_WIN32)
    base_ = VirtualAlloc(nullptr, mappedSize_, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!base_) throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "VirtualAlloc");
#else
    base_ = mmap(nullptr, mappedSize_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base_ == MAP_FAILED) {
        base_ = nullptr;
        throw std::system_error(errno, std::generic_category(), "mmap");
    }
#endif
}
GuestVirtualMemory::~GuestVirtualMemory() {
#if defined(_WIN32)
    if (base_) VirtualFree(base_, 0, MEM_RELEASE);
#else
    if (base_) munmap(base_, mappedSize_);
#endif
}
void GuestVirtualMemory::CheckRange(std::size_t offset, std::size_t bytes) const {
    if (offset > size_ || bytes > size_ - offset) throw std::out_of_range("Guest memory range");
}
void GuestVirtualMemory::Count(std::size_t offset, std::size_t bytes, bool writing) const {
    if (bytes == 0) return;
    const auto last = (offset + bytes - 1) / pageSize_;
    for (auto page = offset / pageSize_; page <= last; ++page) {
        auto& counter = writing ? counters_[page].writes : counters_[page].reads;
        counter.fetch_add(1, std::memory_order_relaxed);
    }
}
void GuestVirtualMemory::Read(std::size_t offset, std::span<std::byte> destination) const {
    CheckRange(offset, destination.size());
    if (!destination.empty()) std::memcpy(destination.data(), static_cast<const std::byte*>(base_) + offset, destination.size());
    Count(offset, destination.size(), false);
}
void GuestVirtualMemory::Write(std::size_t offset, std::span<const std::byte> source) {
    CheckRange(offset, source.size());
    if (!source.empty()) std::memcpy(static_cast<std::byte*>(base_) + offset, source.data(), source.size());
    Count(offset, source.size(), true);
}
GuestVirtualMemory::PageUse GuestVirtualMemory::Utilization(std::size_t page) const {
    if (page >= pageCount_) throw std::out_of_range("Guest page index");
    return {counters_[page].reads.load(std::memory_order_relaxed), counters_[page].writes.load(std::memory_order_relaxed)};
}
