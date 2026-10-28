#include "OccluderBudget.h"
#include "Patch.h"
#include "Settings.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <unordered_set>
#include <vector>

#include <RE/B/BSOcclusionBox.h>
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
	constexpr std::uint32_t kPlaneCost = 1;  // one NiFrustumPlanes in the BSCompoundFrustum
	constexpr std::uint32_t kBoxCost = 2;    // two NiFrustumPlanes (frustumPlanes[2])

	// Rough size on screen: area as seen from the camera divided by squared distance.
	// Plane corners run around the perimeter (0-1-2-3), so 0-1 and 0-3 are adjacent edges.
	// The cross product of two edges has the length of the area, so its dot product with the
	// direction to the camera gives area * |cos(angle)|, the area facing the camera.
	float PlaneWeight(const RE::BSOcclusionPlane* a_plane, const RE::NiPoint3& a_camera)
	{
		const auto& c = a_plane->corners;
		const auto  normal = (c[1] - c[0]).Cross(c[3] - c[0]);
		const auto  toCamera = a_camera - (c[0] + c[1] + c[2] + c[3]) * 0.25f;
		const float dist = toCamera.Length();
		const float facing = dist > 1.0f ? std::fabs(normal.Dot(toCamera)) / dist : normal.Length();
		return facing / (dist * dist + 1.0f);
	}

	// size holds the half extents. A box shows each of its three face pairs scaled by
	// |cos(angle)| between that face and the view direction. The face normals are the
	// columns of the rotation matrix.
	float BoxWeight(const RE::BSOcclusionBox* a_box, const RE::NiPoint3& a_camera)
	{
		const auto& h = a_box->size;
		const auto  toCamera = a_camera - a_box->translation;
		const float dist = toCamera.Length();
		if (dist <= 1.0f) {
			return 4.0f * std::max({ h.x * h.y, h.y * h.z, h.z * h.x });
		}
		const auto  view = toCamera * (1.0f / dist);
		const auto& r = a_box->rotation;
		const float cosX = std::fabs(r.entry[0][0] * view.x + r.entry[1][0] * view.y + r.entry[2][0] * view.z);
		const float cosY = std::fabs(r.entry[0][1] * view.x + r.entry[1][1] * view.y + r.entry[2][1] * view.z);
		const float cosZ = std::fabs(r.entry[0][2] * view.x + r.entry[1][2] * view.y + r.entry[2][2] * view.z);
		const float facing = 4.0f * (h.y * h.z * cosX + h.x * h.z * cosY + h.x * h.y * cosZ);
		return facing / (dist * dist + 1.0f);
	}

	// The engine tests boxes for visibility only later (BSCompoundFrustum::AddOccluder),
	// so boxes completely behind the camera must not take budget slots.
	bool BoxInFront(RE::NiCamera* a_camera, const RE::BSOcclusionBox* a_box)
	{
		const auto& h = a_box->size;
		float       x, y, z;
		for (int i = 0; i < 8; ++i) {
			const RE::NiPoint3 local{ i & 1 ? h.x : -h.x, i & 2 ? h.y : -h.y, i & 4 ? h.z : -h.z };
			if (a_camera->WorldPtToScreenPt3(a_box->translation + a_box->rotation * local, x, y, z, 1e-5f)) {
				return true;
			}
		}
		return false;
	}

	struct Candidate
	{
		float                 weight;
		RE::BSOcclusionShape* shape;
		std::uint32_t         cost;
	};

	// Main thread only, reused every frame
	std::vector<Candidate> candidates;
	// Boxes we set to 2 last frame. Only these are reset, so the engine's own states stay untouched.
	std::unordered_set<const RE::BSOcclusionShape*> demotedBoxes;

	// ID 99736: once per frame, sets visibilityState of every occlusion plane (1 = active, 2 = skipped).
	// Boxes are evaluated later when their state is 0.
	std::uintptr_t updateStatesOriginal = 0;

	void UpdateOccluderStates(RE::ShadowSceneNode* a_node, RE::NiCamera* a_camera, void* a_arg3, void* a_arg4)
	{
		reinterpret_cast<decltype(&UpdateOccluderStates)>(updateStatesOriginal)(a_node, a_camera, a_arg3, a_arg4);

		const auto  budget = Settings::maxActiveOccluders;
		const auto* graph = a_node ? a_node->GetRuntimeData().portalGraph : nullptr;
		if (budget == 0 || !graph || !a_camera) {
			return;
		}

		// The list stores pointers (next, prev, value), see BSPortalGraph::List
		const auto& list = reinterpret_cast<const RE::BSPortalGraph::List<RE::BSOcclusionShape>&>(graph->occlusionShapes);
		const auto& camera = a_camera->world.translate;

		candidates.clear();
		std::uint32_t total = 0;
		for (auto* entry = list.first; entry; entry = entry->next) {
			auto* shape = entry->value;
			if (!shape) {
				continue;
			}

			if (shape->IsOcclusionPlane()) {
				if (shape->visibilityState == 1) {
					candidates.push_back({ PlaneWeight(static_cast<RE::BSOcclusionPlane*>(shape), camera), shape, kPlaneCost });
					total += kPlaneCost;
				}
			}
			else if (shape->IsOcclusionBox()) {
				if (shape->visibilityState == 2 && demotedBoxes.contains(shape)) {
					shape->visibilityState = 0;  // undo our own demotion, the engine tests it again
				}
				const auto* box = static_cast<RE::BSOcclusionBox*>(shape);
				if (shape->visibilityState <= 1 && BoxInFront(a_camera, box)) {
					candidates.push_back({ BoxWeight(box, camera), shape, kBoxCost });
					total += kBoxCost;
				}
			}
		}
		demotedBoxes.clear();

		if (total <= budget) {
			return;
		}

		std::sort(candidates.begin(), candidates.end(),
			[](const Candidate& a_lhs, const Candidate& a_rhs) { return a_lhs.weight > a_rhs.weight; });

		std::uint32_t used = 0;
		for (const auto& candidate : candidates) {
			if (used + candidate.cost <= budget) {
				used += candidate.cost;
				continue;
			}
			candidate.shape->visibilityState = 2;
			if (candidate.cost == kBoxCost) {
				demotedBoxes.insert(candidate.shape);
			}
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
			updateStatesOriginal = trampoline.write_call<5>(site, reinterpret_cast<std::uintptr_t>(&UpdateOccluderStates));
		}

		logs::info("Occluder budget installed: at most {} active plane sets, boxes count twice (experimental).", Settings::maxActiveOccluders);
		return true;
	}
}
