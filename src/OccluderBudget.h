#pragma once

namespace OccluderBudget
{
	// Experimental: keeps at most iMaxActiveOccluders occlusion planes active per frame,
	// the largest on screen win. The engine has no size test, so every plane in view counts.
	bool Install();
}
