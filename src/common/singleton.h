#pragma once

#include "common/common.h"

#include <cstddef>
#include <memory>

namespace Common {

template <class T>
class Singleton {
public:
	[[nodiscard]] static T* Instance() {
		alignas(T) static std::byte storage[sizeof(T)];
		static T* const             instance = std::construct_at(reinterpret_cast<T*>(storage));
		return instance;
	}

	KYTY_CLASS_NO_COPY(Singleton);

protected:
	Singleton()  = default;
	~Singleton() = default;
};

} // namespace Common
