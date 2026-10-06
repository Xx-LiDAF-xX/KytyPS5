#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

class GuestVirtualMemory final {
public:
    struct PageUse { std::uint32_t reads, writes; };
    explicit GuestVirtualMemory(std::size_t bytes);
    ~GuestVirtualMemory();
    GuestVirtualMemory(const GuestVirtualMemory&) = delete;
    GuestVirtualMemory& operator=(const GuestVirtualMemory&) = delete;
    void Read(std::size_t offset, std::span<std::byte> destination) const;
    void Write(std::size_t offset, std::span<const std::byte> source);
    [[nodiscard]] PageUse Utilization(std::size_t page) const;
    [[nodiscard]] std::size_t PageSize() const noexcept { return pageSize_; }
    [[nodiscard]] std::size_t PageCount() const noexcept { return pageCount_; }
    [[nodiscard]] const void* BaseAddress() const noexcept { return base_; }
private:
    struct Counters { std::atomic<std::uint32_t> reads{0}, writes{0}; };
    void CheckRange(std::size_t offset, std::size_t bytes) const;
    void Count(std::size_t offset, std::size_t bytes, bool writing) const;
    void* base_ = nullptr;
    std::size_t size_, mappedSize_, pageSize_, pageCount_;
    std::unique_ptr<Counters[]> counters_;
};
