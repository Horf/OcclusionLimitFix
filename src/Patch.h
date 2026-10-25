#pragma once

#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <span>

namespace Patch
{
	// True if the original code is still in place, so we never overwrite another mod's patch.
	inline bool BytesMatch(std::uintptr_t a_address, std::span<const std::uint8_t> a_expected)
	{
		return std::memcmp(reinterpret_cast<const void*>(a_address), a_expected.data(), a_expected.size()) == 0;
	}
}
