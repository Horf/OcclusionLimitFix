-- include subprojects
includes("lib/commonlibsse-ng")

-- set project constants
set_project("OcclusionLimitFix")
set_version("1.0.0")
set_license("GPL-3.0")
set_languages("c++23")
set_warnings("allextra")

-- add common rules
add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

-- define targets
target("OcclusionLimitFix")
    add_deps("commonlibsse-ng")

    add_rules("commonlibsse-ng.plugin", {
        name = "OcclusionLimitFix",
        author = "PRieST47",
        description = "Prevents the crash caused by too many occlusion planes and boxes"
    })

    -- keep <Windows.h> from defining min/max macros
    add_defines("NOMINMAX")

    -- add src files
    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")
