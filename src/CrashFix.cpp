#include "CrashFix.h"
#include "Patch.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <vector>

#include <RE/B/BSCompoundFrustum.h>

#include <RE/N/NiFrustumPlanes.h>

#include <REL/Relocation.h>

#include <SKSE/Impl/PCH.h>
#include <SKSE/Logger.h>
#include <SKSE/Trampoline.h>

namespace
{
	// All four callers use a uint32[256] stack buffer:
	// entry 0 = view frustum, entries 1-255 = planes[0]-planes[254].
	constexpr std::uint32_t kBufferEntries = 256;
	constexpr std::uint32_t kPlanesInBuffer = kBufferEntries - 1;

	// Get/Set pairs nest only as deep as the scene graph. Deeper means a caller broke the pairing.
	constexpr std::size_t kMaxFrameDepth = 1024;

	// Plane states that do not fit into the caller's buffer, one stack per thread.
	// Each Get pushes a frame, the matching Set pops it (every Get is followed by exactly one Set).
	thread_local std::vector<std::uint32_t> overflowStates;
	thread_local std::vector<std::size_t>   frameStarts;

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
					logs::debug("New maximum: {} plane sets, {} spilled (would have crashed before)", a_used, a_used - kPlanesInBuffer);
				}
				return;
			}
		}
	}

	// Replaces ID 75075
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

	// Replaces ID 75076
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
			return;  // Set without Get, nothing was spilled
		}

		const auto start = frameStarts.back();
		frameStarts.pop_back();

		for (std::uint32_t i = inBuffer; i < used; ++i) {
			const auto index = start + (static_cast<std::size_t>(i) - inBuffer);
			if (index >= overflowStates.size()) {
				break;
			}
			a_frustum->planes[i].activePlanes = static_cast<ActivePlane>(overflowStates[index]);
		}

		overflowStates.resize(start);
	}
}

namespace CrashFix
{
	bool Install()
	{
		const REL::Relocation<std::uintptr_t> getTarget{ RELOCATION_ID(75075, 76843) };
		const REL::Relocation<std::uintptr_t> setTarget{ RELOCATION_ID(75076, 76844) };

		// mov eax, [rcx+0x90]
		constexpr std::array<std::uint8_t, 6> getExpected{ 0x8B, 0x81, 0x90, 0x00, 0x00, 0x00 };
		// mov r8d, [rcx+0xB8]
		constexpr std::array<std::uint8_t, 7> setExpected{ 0x44, 0x8B, 0x81, 0xB8, 0x00, 0x00, 0x00 };

		if (!Patch::BytesMatch(getTarget.address(), getExpected) || !Patch::BytesMatch(setTarget.address(), setExpected)) {
			logs::error("Crash fix: unexpected code in Get/SetActivePlaneState (patched by another mod?), not patching.");
			return false;
		}

		auto& trampoline = SKSE::GetTrampoline();
		trampoline.write_branch<5>(getTarget.address(), &GetActivePlaneState);
		trampoline.write_branch<5>(setTarget.address(), &SetActivePlaneState);

		logs::info("Crash fix installed, occluder limit of {} plane sets lifted.", kPlanesInBuffer);
		return true;
	}
}
