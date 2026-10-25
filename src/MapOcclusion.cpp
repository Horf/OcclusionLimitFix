#include "MapOcclusion.h"
#include "Patch.h"

#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdint>

#include <RE/B/BSMultiBound.h>
#include <RE/B/BSMultiBoundShape.h>
#include <RE/B/BSOcclusionShape.h>
#include <RE/B/BSTEvent.h>

#include <RE/M/MapMenu.h>
#include <RE/M/MenuOpenCloseEvent.h>

#include <RE/N/NiPoint3.h>

#include <RE/U/UI.h>

#include <REL/ID.h>
#include <REL/Module.h>

#include <SKSE/Version.h>
#include <SKSE/Logger.h>
#include <SKSE/Trampoline.h>

namespace
{
	// MapMenu covers the world map and the local map.
	std::atomic<bool> mapOpen{ false };

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
				mapOpen.store(a_event->opening, std::memory_order_relaxed);
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	// Both occluder loops run per object from ID 31398.
	// Loop 1, ID 74649: one bound against one occluder, false if hidden.
	std::uintptr_t occluderTestOriginal = 0;

	bool OccluderTest(RE::BSMultiBoundShape* a_shape, const RE::NiPoint3* a_cameraPos, void* a_cullingProcess, RE::BSOcclusionShape* a_occluder)
	{
		if (mapOpen.load(std::memory_order_relaxed)) {
			return true;
		}
		return reinterpret_cast<decltype(&OccluderTest)>(occluderTestOriginal)(a_shape, a_cameraPos, a_cullingProcess, a_occluder);
	}

	// Loop 2, ID 99787: one bound against all occluders, false if hidden.
	std::uintptr_t boundVisibleOriginal = 0;

	bool BoundVisible(const RE::NiPoint3* a_cameraPos, void* a_cullingProcess, RE::BSMultiBoundShape* a_shape)
	{
		if (mapOpen.load(std::memory_order_relaxed)) {
			return true;
		}
		return reinterpret_cast<decltype(&BoundVisible)>(boundVisibleOriginal)(a_cameraPos, a_cullingProcess, a_shape);
	}

	// ID 99786: wrapper that passes a_multiBound->data to ID 99787.
	std::uintptr_t multiBoundVisibleOriginal = 0;

	bool MultiBoundVisible(const RE::NiPoint3* a_cameraPos, void* a_cullingProcess, RE::BSMultiBound* a_multiBound)
	{
		if (mapOpen.load(std::memory_order_relaxed)) {
			return true;
		}
		return reinterpret_cast<decltype(&MultiBoundVisible)>(multiBoundVisibleOriginal)(a_cameraPos, a_cullingProcess, a_multiBound);
	}

	struct CallSite
	{
		std::uint64_t               functionId;  // SE address library ID of the calling function
		std::ptrdiff_t              offset;
		std::array<std::uint8_t, 5> bytes;
		std::uintptr_t              hook;
		std::uintptr_t*             original;
	};
}

namespace MapOcclusion
{
	bool Install()
	{
		if (REL::Module::get().version() != SKSE::RUNTIME_SSE_1_5_97) {
			logs::warn("Map occlusion skip is only available for 1.5.97 so far, not enabled.");
			return false;
		}

		const auto occluderTest = reinterpret_cast<std::uintptr_t>(&OccluderTest);
		const auto boundVisible = reinterpret_cast<std::uintptr_t>(&BoundVisible);
		const auto multiBoundVisible = reinterpret_cast<std::uintptr_t>(&MultiBoundVisible);

		const std::array<CallSite, 5> sites{ {
			{ 99738, 0xBF, { 0xE8, 0x0C, 0xAF, 0xA8, 0xFF }, occluderTest, &occluderTestOriginal },
			{ 31398, 0x15D, { 0xE8, 0xFE, 0x95, 0xDF, 0x00 }, boundVisible, &boundVisibleOriginal },
			{ 99788, 0x4B, { 0xE8, 0x10, 0xFE, 0xFF, 0xFF }, boundVisible, &boundVisibleOriginal },
			{ 31398, 0x1E9, { 0xE8, 0x62, 0x95, 0xDF, 0x00 }, multiBoundVisible, &multiBoundVisibleOriginal },
			{ 39262, 0x10A, { 0xE8, 0xE1, 0xB4, 0xC2, 0x00 }, multiBoundVisible, &multiBoundVisibleOriginal },
		} };

		// All or nothing: check every site before patching any.
		for (const auto& site : sites) {
			if (!Patch::BytesMatch(REL::ID(site.functionId).address() + site.offset, site.bytes)) {
				logs::error("Map occlusion skip: unexpected code in ID {} (patched by another mod?), not patching.", site.functionId);
				return false;
			}
		}

		auto& trampoline = SKSE::GetTrampoline();
		for (const auto& site : sites) {
			*site.original = trampoline.write_call<5>(REL::ID(site.functionId).address() + site.offset, site.hook);
		}

		logs::info("Map occlusion skip installed (experimental).");
		return true;
	}

	void RegisterEventSinks()
	{
		if (auto* ui = RE::UI::GetSingleton()) {
			ui->AddEventSink<RE::MenuOpenCloseEvent>(MapMenuWatcher::GetSingleton());
		}
	}
}
