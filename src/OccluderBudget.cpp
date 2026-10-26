#include "OccluderBudget.h"
#include "Patch.h"
#include "Settings.h"

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <vector>

#include <RE/B/BSOcclusionPlane.h>
#include <RE/B/BSOcclusionShape.h>
#include <RE/B/BSPortalGraph.h>

#include <RE/N/NiCamera.h>
#include <RE/N/NiPoint3.h>

#include <RE/S/ShadowSceneNode.h>

#include <REL/ID.h>
#include <REL/Module.h>
#include <REL/Version.h>

#include <SKSE/Logger.h>
#include <SKSE/Trampoline.h>

namespace
{
	// Area divided by squared distance to the camera, roughly the plane's size on screen.
	// Corners run around the perimeter (0-1-2-3), so 0-1 and 0-3 are adjacent edges.
	float ScreenWeight(const RE::BSOcclusionPlane* a_plane, const RE::NiPoint3& a_camera)
	{
		const auto& c = a_plane->corners;
		const float area = (c[1] - c[0]).Cross(c[3] - c[0]).Length();
		const auto  center = (c[0] + c[1] + c[2] + c[3]) * 0.25f;
		return area / ((center - a_camera).SqrLength() + 1.0f);
	}

	struct Candidate
	{
		float                  weight;
		RE::BSOcclusionShape*  shape;
	};

	std::vector<Candidate> candidates;  // main thread only, reused every frame

	// ID 99736: once per frame, sets visibilityState of every occlusion plane
	// (1 = active, 2 = skipped) from the view frustum and facing tests.
	std::uintptr_t updatePlaneStatesOriginal = 0;

	void UpdatePlaneStates(RE::ShadowSceneNode* a_node, RE::NiCamera* a_camera, void* a_arg3, void* a_arg4)
	{
		reinterpret_cast<decltype(&UpdatePlaneStates)>(updatePlaneStatesOriginal)(a_node, a_camera, a_arg3, a_arg4);

		const auto  budget = Settings::maxActiveOccluders;
		const auto* graph = a_node ? a_node->GetRuntimeData().portalGraph : nullptr;
		if (budget == 0 || !graph || !a_camera) {
			return;
		}

		// The list stores pointers (next, prev, value), see BSPortalGraph::List
		const auto& list = reinterpret_cast<const RE::BSPortalGraph::List<RE::BSOcclusionShape>&>(graph->occlusionShapes);
		const auto& camera = a_camera->world.translate;

		candidates.clear();
		for (auto* entry = list.first; entry; entry = entry->next) {
			auto* shape = entry->value;
			if (shape && shape->visibilityState == 1 && shape->IsOcclusionPlane()) {
				candidates.push_back({ ScreenWeight(static_cast<RE::BSOcclusionPlane*>(shape), camera), shape });
			}
		}

		if (candidates.size() <= budget) {
			return;
		}

		std::nth_element(candidates.begin(), candidates.begin() + budget, candidates.end(),
			[](const Candidate& a_lhs, const Candidate& a_rhs) { return a_lhs.weight > a_rhs.weight; });

		for (auto it = candidates.begin() + budget; it != candidates.end(); ++it) {
			it->shape->visibilityState = 2;
		}
	}
}

namespace OccluderBudget
{
	bool Install()
	{
		const auto target = REL::RelocationID(99736, 106380).address();

		// SE has one caller, AE the same one plus an inlined copy.
		// Verified in Ghidra on 1.5.97, 1.6.640, 1.6.1170 and 1.7.104.
		std::vector<std::uintptr_t> sites;
		if (REL::Module::IsAE()) {
			// Main::Update (36564) got bigger in 1.7, the inlined call moved by 0x12 bytes
			const std::ptrdiff_t mainUpdateOffset = REL::Module::get().version() >= REL::Version(1, 7, 0, 0) ? 0x972 : 0x960;
			sites = { REL::ID(36574).address() + 0x7C, REL::ID(36564).address() + mainUpdateOffset };
		}
		else {
			sites = { REL::ID(35571).address() + 0x7C };
		}

		for (const auto site : sites) {
			if (!Patch::IsCallTo(site, target)) {
				logs::error("Occluder budget: unexpected code at +0x{:X} (patched by another mod?), not patching.",
					site - REL::Module::get().base());
				return false;
			}
		}

		auto& trampoline = SKSE::GetTrampoline();
		for (const auto site : sites) {
			updatePlaneStatesOriginal = trampoline.write_call<5>(site, reinterpret_cast<std::uintptr_t>(&UpdatePlaneStates));
		}

		logs::info("Occluder budget installed: at most {} active occlusion planes (experimental).", Settings::maxActiveOccluders);
		return true;
	}
}
