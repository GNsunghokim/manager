workspace "Penguin"
configurations { "Debug", "Release" }

configurations "Debug"
flags { "Symbols" }

configurations "Release"
flags { "Optimize" }

language "C"
warnings "Extra"
buildoptions { "-DLINUX -std=gnu99", "-mcmodel=large", "-Wno-unused", "-Wno-format", "-Wno-unused-result" }
linkoptions { "-nostartfiles", "-Wl,-Ttext-segment=0xff00000000" }

project "pn_assistant"

kind "StaticLib"
location "build"
includedirs { "../../penguin/include", "../../penguin/include/ipv4", "./include"}
files { "src/**.h", "src/**.c" }
targetname "pn_assistant"
targetdir "."
