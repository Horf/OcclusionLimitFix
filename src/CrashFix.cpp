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

	// Plane states that do not fit into the caller's buffer, one stack per thread. A frame is only
	// added when a Get actually spills, and is matched to its Set by the caller's buffer address.
	struct Frame
	{
		std::uintptr_t buffer;
		std::size_t    start;  // first entry in overflowStates
	};
	thread_local std::vector<std::uint32_t> overflowStates;
	thread_local std::vector<Frame>         frames;

	// Drops frames of calls deeper than a_buffer (and a_buffer itself if a_inclusive).
	// Those belong to calls that never restored their state.
	// The stack grows downwards, so frames of deeper calls have lower addresses.
	void DropFrames(std::uintptr_t a_buffer, bool a_inclusive)
	{
		while (!frames.empty() && (frames.back().buffer < a_buffer || (a_inclusive && frames.back().buffer == a_buffer))) {
			overflowStates.resize(frames.back().start);
			frames.pop_back();
		}
	}

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
				else {
					logs::debug("New maximum: {} plane sets", a_used);
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

		ReportUsage(used);

		if (used == inBuffer) {
			return;  // everything fits, nothing to spill
		}

		const auto buffer = reinterpret_cast<std::uintptr_t>(a_out);
		DropFrames(buffer, true);
		frames.push_back({ buffer, overflowStates.size() });
		for (std::uint32_t i = inBuffer; i < used; ++i) {
			overflowStates.push_back(a_frustum->planes[i].activePlanes.underlying());
		}
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

		if (frames.empty()) {
			return;  // nothing spilled on this thread
		}

		const auto buffer = reinterpret_cast<std::uintptr_t>(a_in);
		DropFrames(buffer, false);
		if (frames.empty() || frames.back().buffer != buffer) {
			return;  // the matching Get did not spill
		}

		// Plane sets added since the Get keep their current state.
		const auto start = frames.back().start;
		for (std::size_t i = 0; start + i < overflowStates.size() && inBuffer + i < used; ++i) {
			a_frustum->planes[inBuffer + i].activePlanes = static_cast<ActivePlane>(overflowStates[start + i]);
		}
		overflowStates.resize(start);
		frames.pop_back();
	}

	// The first instruction is a jump: another plugin already took over the function
	// (E9 = jmp rel32 as written by Detours, FF 25 = jmp [rip] as written by the SKSE trampoline).
	bool IsRedirected(std::uintptr_t a_address)
	{
		const auto* code = reinterpret_cast<const std::uint8_t*>(a_address);
		return code[0] == 0xE9 || (code[0] == 0xFF && code[1] == 0x25);
	}
}

namespace CrashFix
{
	Result Install()
	{
		const REL::Relocation<std::uintptr_t> getTarget{ RELOCATION_ID(75075, 76843) };
		const REL::Relocation<std::uintptr_t> setTarget{ RELOCATION_ID(75076, 76844) };

		// mov eax, [rcx+0x90]
		constexpr std::array<std::uint8_t, 6> getExpected{ 0x8B, 0x81, 0x90, 0x00, 0x00, 0x00 };
		// mov r8d, [rcx+0xB8]
		constexpr std::array<std::uint8_t, 7> setExpected{ 0x44, 0x8B, 0x81, 0xB8, 0x00, 0x00, 0x00 };

		if (!Patch::BytesMatch(getTarget.address(), getExpected) || !Patch::BytesMatch(setTarget.address(), setExpected)) {
			if (IsRedirected(getTarget.address()) && IsRedirected(setTarget.address())) {
				logs::info("Crash fix: Get/SetActivePlaneState are already redirected by another plugin (e.g. Creation Engine Tweaks), using that instead.");
				return Result::kOtherPlugin;
			}
			logs::error("Crash fix: unexpected code in Get/SetActivePlaneState, no fix applied.");
			return Result::kFailed;
		}

		auto& trampoline = SKSE::GetTrampoline();
		trampoline.write_branch<5>(getTarget.address(), &GetActivePlaneState);
		trampoline.write_branch<5>(setTarget.address(), &SetActivePlaneState);

		logs::info("Crash fix installed, occluder limit of {} plane sets lifted.", kPlanesInBuffer);
		return Result::kInstalled;
	}
}
