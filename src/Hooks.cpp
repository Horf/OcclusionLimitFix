#include "Hooks.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include <REL/ID.h>
#include <REL/Module.h>
#include <REL/Relocation.h>

#include <SKSE/Impl/PCH.h>
#include <SKSE/Logger.h>
#include <SKSE/Trampoline.h>
#include <SKSE/Version.h>

#include <RE/B/BSCompoundFrustum.h>
#include <RE/B/BSMultiBound.h>
#include <RE/B/BSMultiBoundShape.h>
#include <RE/B/BSOcclusionPlane.h>
#include <RE/B/BSOcclusionShape.h>
#include <RE/B/BSTEvent.h>

#include <RE/M/MapMenu.h>
#include <RE/M/MenuOpenCloseEvent.h>

#include <RE/N/NiFrustumPlanes.h>
#include <RE/N/NiPoint3.h>

#include <RE/U/UI.h>

namespace
{
	// Helpers

	// Checks that the original code is still in place, so we never
	// overwrite another mod's patch.
	bool BytesMatch(std::uintptr_t a_address, std::span<const std::uint8_t> a_expected)
	{
		return std::memcmp(reinterpret_cast<const void*>(a_address), a_expected.data(), a_expected.size()) == 0;
	}

	// Redirects one 5-byte call after checking the original bytes.
	// Returns the original call target, or 0 if the code did not match.
	std::uintptr_t HookCall(std::uintptr_t a_callSite, std::span<const std::uint8_t> a_expected, std::uintptr_t a_destination, std::string_view a_name)
	{
		if (!BytesMatch(a_callSite, a_expected)) {
			logs::error("{}: unexpected code at call site, hook skipped.", a_name);
			return 0;
		}
		return SKSE::GetTrampoline().write_call<5>(a_callSite, a_destination);
	}

	// Profiling: counts events and writes "<name>: Xk per second" every few seconds (debug level).
	// A counter that never fires writes no line at all.
	constexpr auto kReportInterval = std::chrono::seconds(5);

	std::int64_t NowNanos()
	{
		using namespace std::chrono;
		return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
	}

	class RateCounter
	{
	public:
		explicit RateCounter(std::string_view a_name) :
			_name(a_name)
		{}

		void Add(std::uint64_t a_count)
		{
			_count.fetch_add(a_count, std::memory_order_relaxed);

			const auto now = NowNanos();
			auto       last = _lastReport.load(std::memory_order_relaxed);
			if (now - last < std::chrono::nanoseconds(kReportInterval).count()) {
				return;
			}
			if (!_lastReport.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
				return;  // another thread is reporting
			}
			const auto count = _count.exchange(0, std::memory_order_relaxed);
			if (last == 0) {
				return;  // first batch only starts the clock
			}
			const auto seconds = static_cast<double>(now - last) / 1e9;
			logs::debug("{}: {:.0f}k per second", _name, static_cast<double>(count) / 1e3 / seconds);
		}

	private:
		std::string_view           _name;
		std::atomic<std::uint64_t> _count{ 0 };
		std::atomic<std::int64_t>  _lastReport{ 0 };
	};

	// Counts per thread and publishes in batches, so counting stays cheap in hot loops.
	void Count(std::uint32_t& a_local, RateCounter& a_counter)
	{
		if (++a_local == 1024) {
			a_local = 0;
			a_counter.Add(1024);
		}
	}

	// Crash fix: replacements for GetActivePlaneState / SetActivePlaneState

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
			const auto index = start + (static_cast<std::size_t>(i) - inBuffer);
			if (index >= overflowStates.size()) {
				break;
			}
			a_frustum->planes[i].activePlanes = static_cast<ActivePlane>(overflowStates[index]);
		}

		overflowStates.resize(start);
	}

	// Experiment, stage 2: skip redundant occlusion plane frustum rebuilds

	// Loop 2 (ID 99787) calls BSOcclusionPlane::UpdateFrustum (ID 74695) for every
	// object and occluder, although the result only depends on the camera position.
	using UpdateFrustum_t = void(RE::BSOcclusionPlane*, const RE::NiPoint3*);

	std::uintptr_t updateFrustumOriginal = 0;

	// The edge planes (left, right, top, bottom) are built through the camera, so the
	// camera lies on all four of them as long as the frustum is current.
	constexpr float kOnPlaneTolerance = 0.5f;    // world units, about 7 mm
	constexpr float kMinNormalLengthSq = 0.25f;  // rejects never-built or degenerate planes

	bool FrustumMatchesCamera(const RE::BSOcclusionPlane* a_plane, const RE::NiPoint3& a_camera)
	{
		const auto& planes = a_plane->frustumPlanes.cullingPlanes;
		for (std::size_t i = 2; i <= 5; ++i) {  // kLeft, kRight, kTop, kBottom
			const auto& n = planes[i].normal;
			if (n.x * n.x + n.y * n.y + n.z * n.z < kMinNormalLengthSq) {
				return false;
			}
			const float distance = n.x * a_camera.x + n.y * a_camera.y + n.z * a_camera.z - planes[i].constant;
			if (std::fabs(distance) > kOnPlaneTolerance) {
				return false;
			}
		}
		return true;
	}

	// Mirrors the end of UpdateFrustum: all six planes, far plane off, flagged edges off.
	std::uint32_t ExpectedActivePlanes(const RE::BSOcclusionPlane* a_plane)
	{
		std::uint32_t mask = 0x3F & ~0x2u;
		for (std::uint32_t edge = 0; edge < 4; ++edge) {
			if (a_plane->edgeFlags[edge] == 1) {
				mask &= ~(0x4u << edge);
			}
		}
		return mask;
	}

	RateCounter                updateFrustumCalls{ "UpdateFrustum calls (loop 2)" };
	RateCounter                updateFrustumRebuilds{ "UpdateFrustum rebuilds (loop 2)" };
	thread_local std::uint32_t updateFrustumLocalCalls = 0;
	thread_local std::uint32_t updateFrustumLocalRebuilds = 0;

	void UpdateFrustumCached(RE::BSOcclusionPlane* a_plane, const RE::NiPoint3* a_cameraPos)
	{
		Count(updateFrustumLocalCalls, updateFrustumCalls);

		if (FrustumMatchesCamera(a_plane, *a_cameraPos)) {
			a_plane->frustumPlanes.activePlanes = static_cast<RE::NiFrustumPlanes::ActivePlane>(ExpectedActivePlanes(a_plane));
			return;
		}

		Count(updateFrustumLocalRebuilds, updateFrustumRebuilds);
		reinterpret_cast<UpdateFrustum_t*>(updateFrustumOriginal)(a_plane, a_cameraPos);
	}

	// Experiment, stage 3: skip all occluder tests while the map is open

	// Seen from above, occluders hide almost nothing, but two loops still walk the
	// whole occluder list for every object. Both are called from ID 31398 per object:
	//   loop 1: ID 99738 -> ID 74649 (one object against one occluder)
	//   loop 2: ID 99787 (one object against all occluders), also via wrapper ID 99786
	// Note: MapMenu also hosts the local map.
	std::atomic<bool> mapMenuOpen{ false };

	class MapMenuWatcher : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		static MapMenuWatcher* GetSingleton()
		{
			static MapMenuWatcher singleton;
			return &singleton;
		}

		RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
		{
			if (a_event && a_event->menuName == RE::MapMenu::MENU_NAME) {
				mapMenuOpen.store(a_event->opening, std::memory_order_relaxed);
				logs::debug("MapMenu {}", a_event->opening ? "opened" : "closed");
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	RateCounter                loop1Calls{ "Occluder loop 1 calls (ID 74649)" };
	RateCounter                loop2Calls{ "Occluder loop 2 calls (ID 99787)" };
	thread_local std::uint32_t loop1LocalCalls = 0;
	thread_local std::uint32_t loop2LocalCalls = 0;

	// Loop 1: ID 74649, returns false if the occluder hides the bound.
	using BoundNotHiddenBy_t = bool(RE::BSMultiBoundShape*, const RE::NiPoint3*, void*, RE::BSOcclusionShape*);
	std::uintptr_t boundNotHiddenByOriginal = 0;

	bool BoundNotHiddenByHook(RE::BSMultiBoundShape* a_shape, const RE::NiPoint3* a_cameraPos, void* a_cullingProcess, RE::BSOcclusionShape* a_occluder)
	{
		Count(loop1LocalCalls, loop1Calls);
		if (mapMenuOpen.load(std::memory_order_relaxed)) {
			return true;
		}
		return reinterpret_cast<BoundNotHiddenBy_t*>(boundNotHiddenByOriginal)(a_shape, a_cameraPos, a_cullingProcess, a_occluder);
	}

	// Loop 2: ID 99787, returns true if the bound is visible, false if an occluder hides it.
	using IsBoundVisible_t = bool(const RE::NiPoint3*, void*, RE::BSMultiBoundShape*);
	// Wrapper ID 99786, passes a_multiBound->data on to ID 99787.
	using IsMultiBoundVisible_t = bool(const RE::NiPoint3*, void*, RE::BSMultiBound*);

	std::uintptr_t isBoundVisibleOriginal = 0;
	std::uintptr_t isMultiBoundVisibleOriginal = 0;

	bool IsBoundVisibleHook(const RE::NiPoint3* a_cameraPos, void* a_cullingProcess, RE::BSMultiBoundShape* a_shape)
	{
		Count(loop2LocalCalls, loop2Calls);
		if (mapMenuOpen.load(std::memory_order_relaxed)) {
			return true;
		}
		return reinterpret_cast<IsBoundVisible_t*>(isBoundVisibleOriginal)(a_cameraPos, a_cullingProcess, a_shape);
	}

	bool IsMultiBoundVisibleHook(const RE::NiPoint3* a_cameraPos, void* a_cullingProcess, RE::BSMultiBound* a_multiBound)
	{
		Count(loop2LocalCalls, loop2Calls);
		if (mapMenuOpen.load(std::memory_order_relaxed)) {
			return true;
		}
		return reinterpret_cast<IsMultiBoundVisible_t*>(isMultiBoundVisibleOriginal)(a_cameraPos, a_cullingProcess, a_multiBound);
	}

	void InstallMapExperiment()
	{
		// The offsets inside these functions are only known for 1.5.97.
		if (REL::Module::get().version() != SKSE::RUNTIME_SSE_1_5_97) {
			logs::debug("Map experiment hooks are 1.5.97 only, skipped.");
			return;
		}

		// Stage 2: UpdateFrustum call inside loop 2
		constexpr std::array<std::uint8_t, 5> call99787{ 0xE8, 0x8A, 0xAA, 0xA8, 0xFF };  // 1412c1411
		if (const auto original = HookCall(REL::ID(99787).address() + 0xC1, call99787,
			reinterpret_cast<std::uintptr_t>(&UpdateFrustumCached), "UpdateFrustum")) {
			updateFrustumOriginal = original;
		}

		// Stage 3, loop 1: call of ID 74649 inside ID 99738
		constexpr std::array<std::uint8_t, 5> call99738{ 0xE8, 0x0C, 0xAF, 0xA8, 0xFF };  // 1412bd68f
		if (const auto original = HookCall(REL::ID(99738).address() + 0xBF, call99738,
			reinterpret_cast<std::uintptr_t>(&BoundNotHiddenByHook), "BoundNotHiddenBy")) {
			boundNotHiddenByOriginal = original;
		}

		// Stage 3, loop 2: callers of ID 99787 and of its wrapper ID 99786
		constexpr std::array<std::uint8_t, 5> call31398a{ 0xE8, 0xFE, 0x95, 0xDF, 0x00 };  // 1404c7d4d
		constexpr std::array<std::uint8_t, 5> call99788{ 0xE8, 0x10, 0xFE, 0xFF, 0xFF };   // 1412c153b
		constexpr std::array<std::uint8_t, 5> call31398b{ 0xE8, 0x62, 0x95, 0xDF, 0x00 };  // 1404c7dd9
		constexpr std::array<std::uint8_t, 5> call39262{ 0xE8, 0xE1, 0xB4, 0xC2, 0x00 };   // 140695e5a

		const auto boundHook = reinterpret_cast<std::uintptr_t>(&IsBoundVisibleHook);
		const auto multiBoundHook = reinterpret_cast<std::uintptr_t>(&IsMultiBoundVisibleHook);

		if (const auto original = HookCall(REL::ID(31398).address() + 0x15D, call31398a, boundHook, "IsBoundVisible #1")) {
			isBoundVisibleOriginal = original;
		}
		if (const auto original = HookCall(REL::ID(99788).address() + 0x4B, call99788, boundHook, "IsBoundVisible #2")) {
			isBoundVisibleOriginal = original;
		}
		if (const auto original = HookCall(REL::ID(31398).address() + 0x1E9, call31398b, multiBoundHook, "IsMultiBoundVisible #1")) {
			isMultiBoundVisibleOriginal = original;
		}
		if (const auto original = HookCall(REL::ID(39262).address() + 0x10A, call39262, multiBoundHook, "IsMultiBoundVisible #2")) {
			isMultiBoundVisibleOriginal = original;
		}

		logs::debug("Map experiment hooks installed.");
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
		logs::info("Replaced GetActivePlaneState and SetActivePlaneState, buffer limit of {} plane sets lifted.", kPlanesInBuffer);

		InstallMapExperiment();

		logs::debug("Trampoline: {} of {} bytes used", trampoline.allocated_size(), trampoline.capacity());
		return true;
	}

	void RegisterEventSinks()
	{
		if (auto* ui = RE::UI::GetSingleton()) {
			ui->AddEventSink<RE::MenuOpenCloseEvent>(MapMenuWatcher::GetSingleton());
			logs::debug("Watching MapMenu.");
		}
	}
}
