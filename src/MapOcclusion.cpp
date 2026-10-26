#include "MapOcclusion.h"
#include "Patch.h"

#include <array>
#include <atomic>
#include <cstdarg>

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
		REL::RelocationID caller;
		std::ptrdiff_t    offset;
		REL::RelocationID callee;
		std::uintptr_t    hook;
		std::uintptr_t*   original;

		std::uintptr_t Address() const { return caller.address() + offset; }
	};
}

namespace MapOcclusion
{
	bool Install()
	{
		const auto occluderTest = reinterpret_cast<std::uintptr_t>(&OccluderTest);
		const auto boundVisible = reinterpret_cast<std::uintptr_t>(&BoundVisible);
		const auto multiBoundVisible = reinterpret_cast<std::uintptr_t>(&MultiBoundVisible);

		// Verified in Ghidra on 1.5.97, 1.6.640, 1.6.1170 and 1.7.104
		const std::array<CallSite, 4> sites{ {
			{ REL::RelocationID(99738, 106382), 0xBF, REL::RelocationID(74649, 76380), occluderTest, &occluderTestOriginal },
			{ REL::RelocationID(31398, 32189), 0x15D, REL::RelocationID(99787, 106434), boundVisible, &boundVisibleOriginal },
			{ REL::RelocationID(31398, 32189), REL::Module::IsAE() ? 0x20B : 0x1E9, REL::RelocationID(99786, 106433), multiBoundVisible, &multiBoundVisibleOriginal },
			{ REL::RelocationID(39262, 40335), 0x10A, REL::RelocationID(99786, 106433), multiBoundVisible, &multiBoundVisibleOriginal },
		} };

		// All or nothing: check every site before patching any.
		for (const auto& site : sites) {
			if (!Patch::IsCallTo(site.Address(), site.callee.address())) {
				logs::error("Map occlusion skip: unexpected code at +0x{:X} (patched by another mod?), not patching.",
					site.Address() - REL::Module::get().base());
				return false;
			}
		}

		auto& trampoline = SKSE::GetTrampoline();
		for (const auto& site : sites) {
			*site.original = trampoline.write_call<5>(site.Address(), site.hook);
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
