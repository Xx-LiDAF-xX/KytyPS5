#pragma once

#include <atomic>
#include <vector>
#include <cstdint>
#include <new>

namespace Common {

// Cache-line aligned (usually 64 bytes) to prevent false sharing between producer (guest) and consumer (host)
struct alignas(64) AudioRingBuffer {
	std::vector<int16_t> buffer;
	std::atomic<size_t> head_idx{0}; // Written by Producer (Guest Audio Thread)
	alignas(64) std::atomic<size_t> tail_idx{0}; // Written by Consumer (Host OS Audio API)

	explicit AudioRingBuffer(size_t capacity) : buffer(capacity, 0) {}

	bool Push(const int16_t* data, size_t count) {
		size_t head = head_idx.load(std::memory_order_relaxed);
		size_t tail = tail_idx.load(std::memory_order_acquire); // Ensure we see consumer reads
		
		size_t available = (tail > head) ? (tail - head - 1) : (buffer.size() - head + tail - 1);
		if (available < count) return false; // Buffer full, prevent overflow pops

		for (size_t i = 0; i < count; ++i) {
			buffer[(head + i) % buffer.size()] = data[i];
		}

		// Release semantic: ensure data writes finish before head pointer updates
		head_idx.store((head + count) % buffer.size(), std::memory_order_release);
		return true;
	}

	bool Pop(int16_t* out_data, size_t count) {
		size_t tail = tail_idx.load(std::memory_order_relaxed);
		size_t head = head_idx.load(std::memory_order_acquire); // Ensure we see producer writes

		size_t filled = (head >= tail) ? (head - tail) : (buffer.size() - tail + head);
		if (filled < count) return false; // Underflow, pad with silence if necessary in upper logic

		for (size_t i = 0; i < count; ++i) {
			out_data[i] = buffer[(tail + i) % buffer.size()];
		}

		// Release semantic: ensure data reads finish before tail pointer updates
		tail_idx.store((tail + count) % buffer.size(), std::memory_order_release);
		return true;
	}

	void Reset() {
		head_idx.store(0, std::memory_order_relaxed);
		tail_idx.store(0, std::memory_order_release);
	}
};

} // namespace Common
