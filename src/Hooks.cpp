#include "Hooks.h"

#include <cstdint>
#include <vector>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <cstdarg>
#include <span>
#include <array>

#include <REL/Relocation.h>

#include <SKSE/Impl/PCH.h>
#include <SKSE/Logger.h>
#include <SKSE/Trampoline.h>

#include <RE/B/BSCompoundFrustum.h>

#include <RE/N/NiFrustumPlanes.h>

namespace
{
	// All four callers reserve a buffer of 256 uint32 on the stack
	// (Ghidra: auStack_408 / local_408 / local_438, 1024 bytes each).
	// Entry 0 = view frustum, entries 1 to 255 = planes[0] to planes[254].
	constexpr std::uint32_t kBufferEntries = 256;
	constexpr std::uint32_t kPlanesInBuffer = kBufferEntries - 1;

	// Get/Set pairs only nest as deep as the scene graph. Anything deeper means
	// a caller broke the pairing (possible on runtimes we have not verified).
	constexpr std::size_t kMaxFrameDepth = 1024;

	// Plane states that no longer fit into the caller's buffer, one
	// stack per thread. Every Get pushes a frame and the matching Set
	// pops it. This works because every Get is followed by exactly one
	// Set in all four callers (verified in Ghidra).
	thread_local std::vector<std::uint32_t> overflowStates;
	thread_local std::vector<std::size_t>   frameStarts;

	// Logging only: highest number of plane sets seen so far.
	std::atomic<std::uint32_t> maxUsedSeen{ 0 };

	std::uint32_t UsedPlanes(const RE::BSCompoundFrustum* a_frustum)
	{
		return std::min(a_frustum->freePlane, static_cast<std::uint32_t>(a_frustum->planes.size()));
	}

	void ReportUsage(std::uint32_t a_used)
	{
		auto previous = maxUsedSeen.load(std::memory_order_relaxed);
		while (a_used > previous) {
			if (maxUsedSeen.compare_exchange_weak(previous, a_used, std::memory_order_relaxed)) {
				if (a_used > kPlanesInBuffer) {
					logs::debug("New maximum: {} plane sets, {} of them spilled (would have crashed before)", a_used, a_used - kPlanesInBuffer);
				}
				return;
			}
		}
	}

	// Replacement for BSCompoundFrustum::GetActivePlaneState (ID 75075)
	void GetActivePlaneState(RE::BSCompoundFrustum* a_frustum, std::uint32_t* a_out)
	{
		a_out[0] = a_frustum->viewFrustum.activePlanes.underlying();

		const auto used = UsedPlanes(a_frustum);
		const auto inBuffer = std::min(used, kPlanesInBuffer);

		for (std::uint32_t i = 0; i < inBuffer; ++i) {
			a_out[1 + i] = a_frustum->planes[i].activePlanes.underlying();
		}

		if (frameStarts.size() >= kMaxFrameDepth) {
			static std::atomic_flag reported{};
			if (!reported.test_and_set()) {
				logs::error("Unpaired GetActivePlaneState calls detected, resetting spill stack.");
			}
			frameStarts.clear();
			overflowStates.clear();
		}

		frameStarts.push_back(overflowStates.size());
		for (std::uint32_t i = inBuffer; i < used; ++i) {
			overflowStates.push_back(a_frustum->planes[i].activePlanes.underlying());
		}

		ReportUsage(used);
	}

	// Replacement for BSCompoundFrustum::SetActivePlaneState (ID 75076)
	void SetActivePlaneState(RE::BSCompoundFrustum* a_frustum, std::uint32_t* a_in)
	{
		using ActivePlane = RE::NiFrustumPlanes::ActivePlane;

		a_frustum->viewFrustum.activePlanes = static_cast<ActivePlane>(a_in[0]);

		const auto used = UsedPlanes(a_frustum);
		const auto inBuffer = std::min(used, kPlanesInBuffer);

		for (std::uint32_t i = 0; i < inBuffer; ++i) {
			a_frustum->planes[i].activePlanes = static_cast<ActivePlane>(a_in[1 + i]);
		}

		if (frameStarts.empty()) {
			// Must not happen (Set without a preceding Get), so nothing was spilled.
			return;
		}

		const auto start = frameStarts.back();
		frameStarts.pop_back();

		for (std::uint32_t i = inBuffer; i < used; ++i) {
			const auto index = start + (static_cast<unsigned long long>(i) - inBuffer);
			if (index >= overflowStates.size()) {
				break;
			}
			a_frustum->planes[i].activePlanes = static_cast<ActivePlane>(overflowStates[index]);
		}

		overflowStates.resize(start);
	}

	// Checks that the original code is still in place, so we never
	// overwrite another mod's patch.
	bool BytesMatch(std::uintptr_t a_address, std::span<const std::uint8_t> a_expected)
	{
		return std::memcmp(reinterpret_cast<const void*>(a_address), a_expected.data(), a_expected.size()) == 0;
	}
}

namespace Hooks
{
	bool Install()
	{
		const REL::Relocation<std::uintptr_t> getTarget{ RELOCATION_ID(75075, 76843) };
		const REL::Relocation<std::uintptr_t> setTarget{ RELOCATION_ID(75076, 76844) };

		// mov eax, [rcx+0x90] (viewFrustum.activePlanes), from Ghidra at 140d5ba80
		constexpr std::array<std::uint8_t, 6> getExpected{ 0x8B, 0x81, 0x90, 0x00, 0x00, 0x00 };
		if (!BytesMatch(getTarget.address(), getExpected)) {
			logs::error("GetActivePlaneState does not match the expected code (patched by another mod?). Not patching.");
			return false;
		}

		// mov r8d, [rcx+0xB8] (freePlane), from Ghidra at 140d5bad0
		constexpr std::array<std::uint8_t, 7> setExpected{ 0x44, 0x8B, 0x81, 0xB8, 0x00, 0x00, 0x00 };
		if (!BytesMatch(setTarget.address(), setExpected)) {
			logs::error("SetActivePlaneState does not match the expected code (patched by another mod?). Not patching.");
			return false;
		}

		auto& trampoline = SKSE::GetTrampoline();
		trampoline.write_branch<5>(getTarget.address(), &GetActivePlaneState);
		trampoline.write_branch<5>(setTarget.address(), &SetActivePlaneState);

		logs::debug("Trampoline: {} of {} bytes used", trampoline.allocated_size(), trampoline.capacity());

		logs::info("Replaced GetActivePlaneState and SetActivePlaneState, buffer limit of {} plane sets lifted.", kPlanesInBuffer);
		return true;
	}
}
