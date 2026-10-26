#include "Settings.h"

#include <cctype>
#include <filesystem>
#include <string>

#include <spdlog/common.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace
{
	// Lowercase, cut at the first space or ';' so trailing comments do not matter.
	std::string Normalize(const char* a_value)
	{
		std::string result;
		for (const char* c = a_value; *c != '\0' && *c != ';' && !std::isspace(static_cast<unsigned char>(*c)); ++c) {
			result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(*c))));
		}
		return result;
	}

	std::string Read(const char* a_section, const char* a_key, const char* a_default, const std::string& a_path)
	{
		char buffer[64]{};
		GetPrivateProfileStringA(a_section, a_key, a_default, buffer, sizeof(buffer), a_path.c_str());
		return Normalize(buffer);
	}
}

namespace Settings
{
	void Load()
	{
		const auto path = (std::filesystem::current_path() / "Data/SKSE/Plugins/OcclusionLimitFix.ini").string();

		const auto level = Read("General", "sLogLevel", "info", path);
		if (level == "error") {
			logLevel = spdlog::level::err;
		} else if (level == "warn") {
			logLevel = spdlog::level::warn;
		} else if (level == "info") {
			logLevel = spdlog::level::info;
		} else if (level == "debug") {
			logLevel = spdlog::level::debug;
		} else {
			invalidLogLevel = true;
		}

		const auto skip = Read("Experimental", "bSkipOcclusionOnMap", "false", path);
		skipOcclusionOnMap = skip == "true" || skip == "1";

		maxActiveOccluders = GetPrivateProfileIntA("Experimental", "iMaxActiveOcclusionPlanes", 0, path.c_str());
	}
}
