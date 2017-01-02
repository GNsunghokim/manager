workspace "Penguin"
configurations { "Debug", "Release" }

configurations "Debug"
flags { "Symbols" }

configurations "Release"
flags { "Optimize" }

language "C"
warnings "Extra"
buildoptions { "-DLINUX -std=gnu99", "-mcmodel=large", "-Wno-unused", "-Wno-format", "-Wno-unused-result" }

project "Manager"

kind "ConsoleApp"
location "build"
targetname "test"
targetdir "."
files { "src/**.h", "src/**.c" }
includedirs { "../../../penguin/include", "../../lib/include" }
libdirs { "../../.", "../../lib" }
links { "pn_assistant", "umpn", "rt" }
