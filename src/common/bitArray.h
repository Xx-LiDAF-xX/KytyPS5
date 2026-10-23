#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <utility>
#include <version>
#if __has_include(<simd>) && defined(__cpp_lib_simd)
#include <simd>
#endif

namespace Common {

template <size_t N>
class BitArray final {
	static_assert(N != 0, "BitArray size must be nonzero");
	static_assert(N % 64 == 0, "BitArray size must be a multiple of 64 bits");

	static constexpr size_t BITS_PER_WORD = 64;
	static constexpr size_t WORD_COUNT    = N / BITS_PER_WORD;

public:
	using Range = std::pair<size_t, size_t>;

	class Iterator final {
	public:
		using iterator_category = std::forward_iterator_tag;
		using value_type        = Range;
		using difference_type   = std::ptrdiff_t;
		using pointer           = const Range*;
		using reference         = const Range&;

		Iterator(const BitArray& bits, size_t start)
		    : m_bits(bits), m_range(bits.FirstRangeFrom(start)) {}

		Iterator& operator++() {
			m_range = m_bits.FirstRangeFrom(m_range.second);
			return *this;
		}

		[[nodiscard]] bool operator==(const Iterator& other) const {
			return &m_bits == &other.m_bits && m_range == other.m_range;
		}

		[[nodiscard]] bool operator!=(const Iterator& other) const { return !(*this == other); }

		[[nodiscard]] reference operator*() const { return m_range; }
		[[nodiscard]] pointer   operator->() const { return &m_range; }

	private:
		const BitArray& m_bits;
		Range           m_range;
	};

	using const_iterator = Iterator;

	constexpr BitArray() = default;

	constexpr BitArray(const BitArray& other, size_t start, size_t end) {
		if (start >= end || end > N) {
			return;
		}

		const auto first_word = start / BITS_PER_WORD;
		const auto last_word  = (end - 1) / BITS_PER_WORD;
		const auto start_bit  = start % BITS_PER_WORD;
		const auto end_bit    = (end - 1) % BITS_PER_WORD;
		const auto start_mask = ~uint64_t {0} << start_bit;
		const auto end_mask =
		    end_bit == BITS_PER_WORD - 1 ? ~uint64_t {0} : (uint64_t {1} << (end_bit + 1)) - 1;

		if (first_word == last_word) {
			m_data[first_word] = other.m_data[first_word] & start_mask & end_mask;
			return;
		}

		m_data[first_word] = other.m_data[first_word] & start_mask;
		for (auto word = first_word + 1; word < last_word; word++) {
			m_data[word] = other.m_data[word];
		}
		m_data[last_word] = other.m_data[last_word] & end_mask;
	}

	[[nodiscard]] constexpr bool Get(size_t index) const {
		return (m_data[index / BITS_PER_WORD] & (uint64_t {1} << (index % BITS_PER_WORD))) != 0;
	}

	constexpr void Set(size_t index) {
		m_data[index / BITS_PER_WORD] |= uint64_t {1} << (index % BITS_PER_WORD);
	}

	constexpr void Unset(size_t index) {
		m_data[index / BITS_PER_WORD] &= ~(uint64_t {1} << (index % BITS_PER_WORD));
	}

	constexpr void SetRange(size_t start, size_t end) {
		if (start >= end || end > N) {
			return;
		}

		const auto first_word = start / BITS_PER_WORD;
		const auto last_word  = (end - 1) / BITS_PER_WORD;
		const auto start_bit  = start % BITS_PER_WORD;
		const auto end_bit    = (end - 1) % BITS_PER_WORD;
		const auto start_mask = ~uint64_t {0} << start_bit;
		const auto end_mask =
		    end_bit == BITS_PER_WORD - 1 ? ~uint64_t {0} : (uint64_t {1} << (end_bit + 1)) - 1;

		if (first_word == last_word) {
			m_data[first_word] |= start_mask & end_mask;
			return;
		}

		m_data[first_word] |= start_mask;
		for (auto word = first_word + 1; word < last_word; word++) {
			m_data[word] = ~uint64_t {0};
		}
		m_data[last_word] |= end_mask;
	}

	constexpr void UnsetRange(size_t start, size_t end) {
		if (start >= end || end > N) {
			return;
		}

		const auto first_word = start / BITS_PER_WORD;
		const auto last_word  = (end - 1) / BITS_PER_WORD;
		const auto start_bit  = start % BITS_PER_WORD;
		const auto end_bit    = (end - 1) % BITS_PER_WORD;
		const auto start_mask = (uint64_t {1} << start_bit) - 1;
		const auto end_mask =
		    end_bit == BITS_PER_WORD - 1 ? uint64_t {0} : ~((uint64_t {1} << (end_bit + 1)) - 1);

		if (first_word == last_word) {
			m_data[first_word] &= start_mask | end_mask;
			return;
		}

		m_data[first_word] &= start_mask;
		for (auto word = first_word + 1; word < last_word; word++) {
			m_data[word] = 0;
		}
		m_data[last_word] &= end_mask;
	}

	constexpr void Clear() { m_data.fill(0); }
	constexpr void Fill() { m_data.fill(~uint64_t {0}); }

	[[nodiscard]] constexpr bool None() const {
#if __has_include(<simd>) && defined(__cpp_lib_simd)
		if (!std::is_constant_evaluated()) {
			using SimdWord                    = std::simd<uint64_t>;
			constexpr size_t lanes            = SimdWord::size();
			SimdWord         combined_lanes   = 0;
			size_t           word_idx         = 0;
			for (; word_idx + lanes <= WORD_COUNT; word_idx += lanes) {
				combined_lanes |= SimdWord(&m_data[word_idx], std::element_aligned);
			}
			uint64_t combined = 0;
			for (size_t i = 0; i < lanes; ++i) {
				combined |= combined_lanes[i];
			}
			for (; word_idx < WORD_COUNT; ++word_idx) {
				combined |= m_data[word_idx];
			}
			return combined == 0;
		}
#endif
		uint64_t combined = 0;
		for (const auto word: m_data) {
			combined |= word;
		}
		return combined == 0;
	}

	[[nodiscard]] constexpr bool Any() const { return !None(); }

	static constexpr size_t Words = WORD_COUNT;
	[[nodiscard]] constexpr uint64_t Word(size_t index) const { return m_data[index]; }

	// Whether any bit of [start, end) is set in the words word_at(i) returns: the value of
	// BitArray(bits, start, end).Any() without building the masked copy. False for an empty or
	// out-of-range [start, end), as that copy would be empty.
	template <typename WordAt>
	[[nodiscard]] static constexpr bool AnyInRange(size_t start, size_t end, WordAt&& word_at) {
		if (start >= end || end > N) {
			return false;
		}
		const auto first_word = start / BITS_PER_WORD;
		const auto last_word  = (end - 1) / BITS_PER_WORD;
		const auto end_bit    = (end - 1) % BITS_PER_WORD;
		const auto start_mask = ~uint64_t {0} << (start % BITS_PER_WORD);
		const auto end_mask =
		    end_bit == BITS_PER_WORD - 1 ? ~uint64_t {0} : (uint64_t {1} << (end_bit + 1)) - 1;
		if (first_word == last_word) {
			return (word_at(first_word) & start_mask & end_mask) != 0;
		}
		if ((word_at(first_word) & start_mask) != 0) {
			return true;
		}
		for (auto word = first_word + 1; word < last_word; word++) {
			if (word_at(word) != 0) {
				return true;
			}
		}
		return (word_at(last_word) & end_mask) != 0;
	}

	[[nodiscard]] constexpr bool AnyInRange(size_t start, size_t end) const {
		return AnyInRange(start, end, [this](size_t word) { return m_data[word]; });
	}

	[[nodiscard]] constexpr Range FirstRangeFrom(size_t start) const {
		if (start >= N) {
			return {N, N};
		}

		auto word_index = start / BITS_PER_WORD;
		auto word       = m_data[word_index] & (~uint64_t {0} << (start % BITS_PER_WORD));
		while (word == 0) {
			word_index++;
			if (word_index == WORD_COUNT) {
				return {N, N};
			}
			word = m_data[word_index];
		}

		const auto first     = word_index * BITS_PER_WORD + std::countr_zero(word);
		const auto first_bit = first % BITS_PER_WORD;
		const auto first_ones =
		    static_cast<size_t>(std::countr_one(m_data[word_index] >> first_bit));
		if (first_bit + first_ones < BITS_PER_WORD) {
			return {first, first + first_ones};
		}

		for (word_index++; word_index < WORD_COUNT; word_index++) {
			word = m_data[word_index];
			if (word != ~uint64_t {0}) {
				return {first, word_index * BITS_PER_WORD + std::countr_one(word)};
			}
		}
		return {first, N};
	}

	[[nodiscard]] constexpr Range FirstRange() const { return FirstRangeFrom(0); }

	[[nodiscard]] constexpr Range LastRangeFrom(size_t end) const {
		if (end == 0) {
			return {0, 0};
		}
		if (end > N) {
			end = N;
		}

		auto       word_index = (end - 1) / BITS_PER_WORD;
		const auto end_bit    = (end - 1) % BITS_PER_WORD;
		const auto end_mask =
		    end_bit == BITS_PER_WORD - 1 ? ~uint64_t {0} : (uint64_t {1} << (end_bit + 1)) - 1;
		auto word = m_data[word_index] & end_mask;
		while (word == 0) {
			if (word_index == 0) {
				return {0, 0};
			}
			word = m_data[--word_index];
		}

		const auto empty_bits = static_cast<size_t>(std::countl_zero(word));
		const auto ones       = static_cast<size_t>(std::countl_one(word << empty_bits));
		const auto last       = (word_index + 1) * BITS_PER_WORD - empty_bits;
		if (empty_bits + ones < BITS_PER_WORD) {
			return {last - ones, last};
		}

		while (word_index != 0) {
			word = m_data[--word_index];
			if (word != ~uint64_t {0}) {
				return {(word_index + 1) * BITS_PER_WORD - std::countl_one(word), last};
			}
		}
		return {0, last};
	}

	[[nodiscard]] constexpr Range LastRange() const { return LastRangeFrom(N); }

	[[nodiscard]] const_iterator begin() const { return Iterator(*this, 0); }
	[[nodiscard]] const_iterator end() const { return Iterator(*this, N); }

	constexpr BitArray& operator^=(const BitArray& other) {
		for (size_t word = 0; word < WORD_COUNT; word++) {
			m_data[word] ^= other.m_data[word];
		}
		return *this;
	}

	[[nodiscard]] constexpr BitArray operator^(const BitArray& other) const {
		auto result = *this;
		result ^= other;
		return result;
	}

	[[nodiscard]] constexpr BitArray operator~() const {
		auto result = *this;
		for (auto& word: result.m_data) {
			word = ~word;
		}
		return result;
	}

	constexpr BitArray& operator&=(const BitArray& other) {
		for (size_t word = 0; word < WORD_COUNT; word++) {
			m_data[word] &= other.m_data[word];
		}
		return *this;
	}

	[[nodiscard]] constexpr BitArray operator&(const BitArray& other) const {
		auto result = *this;
		result &= other;
		return result;
	}

	constexpr BitArray& operator|=(const BitArray& other) {
		for (size_t word = 0; word < WORD_COUNT; word++) {
			m_data[word] |= other.m_data[word];
		}
		return *this;
	}

	[[nodiscard]] constexpr BitArray operator|(const BitArray& other) const {
		auto result = *this;
		result |= other;
		return result;
	}

	// Number of set bits.
	[[nodiscard]] constexpr size_t Count() const {
		size_t count = 0;
		for (const auto word: m_data) {
			count += static_cast<size_t>(std::popcount(word));
		}
		return count;
	}

private:
	std::array<uint64_t, WORD_COUNT> m_data {};
};

} // namespace Common
