#pragma once

namespace MapOcclusion
{
	// Experimental: skips occluder tests while the map is open.
	bool Install();

	// Call after kDataLoaded, when the UI event sources exist.
	void RegisterEventSinks();
}
