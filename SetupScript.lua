workspace "URGL-Launcher"
    architecture "x64"
    configurations { "Release" }
    startproject "URGL-Launcher"

outputdir = "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"

project "URGL-Launcher"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++Latest"
    systemversion "latest"
    staticruntime "on" -- Use MT (static runtime)

    targetdir ("Build/" .. outputdir .. "/%{prj.name}")
    objdir ("Build/Intermediates/" .. outputdir .. "/%{prj.name}")

    includedirs {
        "Vendors",
        "Vendors/GLFW/include",
        "Vendors/Chromium/include",
        "Vendors/Chromium/"
    }

    files {
        "Source/**.h",
        "Source/**.cpp",
        "Source/**.hpp",
        "Source/**.c",
        "Vendors/stb_image/**.h",
        "Vendors/stb_image/**.cpp",
        "Vendors/glad.c"
    }

    -- Organize files under a filter called "ImGui" "stb_image"
    vpaths {
        ["Launcher/*"] = { "Source/**.h", "Source/**.cpp" },
        ["Vendors/stb_image/*"] = { "Vendors/stb_image/**.h", "Vendors/stb_image/**.cpp" },
        ["Vendors/glad/*"] = { "Vendors/glad.c"},
        

    }

    libdirs {
        "Vendors/GLFW/lib-vc2022",
        "Vendors/Chromium/Release",
        "Vendors/Chromium/"
    }

    links {
        "glfw3_mt.lib",
        "opengl32.lib",
        "libcef.lib",
        "libcef_dll_wrapper.lib"
    }

    defines {
    }

    -- Ensure Multi-Byte Character Set (do not define UNICODE/_UNICODE)
    filter { }
        defines { }

    filter "system:windows"
        systemversion "latest"
        defines { "PLATFORM_WINDOWS" }
        icon "./Assets/app.ico"
        -- Remove UNICODE defines if present
        removedefines { "UNICODE", "_UNICODE" }
        linkoptions { "/SUBSYSTEM:WINDOWS" }

    filter "configurations:Release"
        defines { "NDEBUG" }
        optimize "On"
        staticruntime "on"

-- Custom post-build commands to copy Assets and configuration files
postbuildcommands {
    "{COPY} ./Assets %{cfg.targetdir}/Assets"
}