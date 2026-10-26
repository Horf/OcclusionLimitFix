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

	// True if a_site holds "call a_target" (E8 rel32). False if another mod already redirected it.
	inline bool IsCallTo(std::uintptr_t a_site, std::uintptr_t a_target)
	{
		if (*reinterpret_cast<const std::uint8_t*>(a_site) != 0xE8) {
			return false;
		}
		std::int32_t rel;
		std::memcpy(&rel, reinterpret_cast<const void*>(a_site + 1), sizeof(rel));
		return static_cast<std::uintptr_t>(static_cast<std::intptr_t>(a_site + 5) + rel) == a_target;
	}
}
