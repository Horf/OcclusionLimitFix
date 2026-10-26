#pragma once

namespace OccluderBudget
{
	// Experimental: keeps at most iMaxActiveOccluders plane sets active per frame (planes count 1,
	// boxes 2), the largest on screen win. The engine has no size test, so every occluder in view counts.
	bool Install();
}
