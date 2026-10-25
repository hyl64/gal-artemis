cmake_minimum_required(VERSION 3.20)
project(artemis-mac CXX C)

# Artemis, the engine of the 2013+ titles the port targets, written from scratch:
# no upstream source exists to port, so every layer is a new implementation held
# to account by tests that build their own fixtures.
#
# This tree starts with the container: the .pfs archives a game ships in.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_ARCHITECTURES arm64)
if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

# src/iet.cpp (the KAG script parser) belongs here rather than in the render layer:
# it depends on CoreFoundation only, and both the container tools and the effects
# tool need it.  Keeping it out of artemis_render means the half-written renderer
# cannot stop them from building.
add_library(artemis STATIC src/pfs.cpp src/sniff.cpp src/pngio.cpp src/lua.cpp
	src/ipt.cpp src/luaast.cpp src/asb.cpp src/ast.cpp src/runtime.cpp src/iet.cpp)

# The .iet script runner and the renderer land as new files during parallel work.
# They live in their own library so that a half-written renderer cannot stop the
# four container/script tools from building: `make pfstool asttool asbtool
# iettool` never touches this library.  CONFIGURE_DEPENDS lets a plain rebuild
# notice a new source.
file(GLOB ARTEMIS_RENDER_SOURCES CONFIGURE_DEPENDS
	src/render/*.cpp)
if(ARTEMIS_RENDER_SOURCES)
	add_library(artemis_render STATIC ${ARTEMIS_RENDER_SOURCES})
	target_link_libraries(artemis_render PUBLIC artemis)
endif()


target_include_directories(artemis PUBLIC src)
target_link_libraries(artemis PUBLIC "-framework CoreFoundation")

# The audio layer and the effects layer land as new directories during parallel
# work, each in its own static library so a half-written layer cannot stop
# anybody else's build.  CONFIGURE_DEPENDS lets a plain rebuild see new files.
file(GLOB ARTEMIS_AUDIO_SOURCES CONFIGURE_DEPENDS src/audio/*.cpp)
if(ARTEMIS_AUDIO_SOURCES)
	add_library(artemis_audio STATIC ${ARTEMIS_AUDIO_SOURCES})
	target_link_libraries(artemis_audio PUBLIC artemis
		"-framework AVFoundation" "-framework AudioToolbox" "-framework CoreAudio")
endif()

file(GLOB ARTEMIS_FX_SOURCES CONFIGURE_DEPENDS src/fx/*.cpp)
if(ARTEMIS_FX_SOURCES)
	add_library(artemis_fx STATIC ${ARTEMIS_FX_SOURCES})
	target_link_libraries(artemis_fx PUBLIC artemis)
endif()

file(GLOB ARTEMIS_SAVE_SOURCES CONFIGURE_DEPENDS src/save/*.cpp)
if(ARTEMIS_SAVE_SOURCES)
	add_library(artemis_save STATIC ${ARTEMIS_SAVE_SOURCES})
	target_link_libraries(artemis_save PUBLIC artemis)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(PNG REQUIRED libpng)
target_include_directories(artemis PUBLIC ${PNG_INCLUDE_DIRS})
target_link_directories(artemis PUBLIC ${PNG_LIBRARY_DIRS})
target_link_libraries(artemis PUBLIC ${PNG_LIBRARIES})

# the tools' self tests use the real samples testdata/ ships when they are
# there; the path comes from the build, not from the working directory
set(ARTEMIS_TESTDATA "${CMAKE_SOURCE_DIR}/../testdata")

add_executable(pfstool tools/pfstool.cpp)
target_compile_definitions(pfstool PRIVATE ARTEMIS_TESTDATA="${ARTEMIS_TESTDATA}")
add_executable(iettool tools/iettool.cpp)
target_compile_definitions(iettool PRIVATE ARTEMIS_TESTDATA="${ARTEMIS_TESTDATA}")
target_link_libraries(iettool PRIVATE artemis)

add_executable(asbtool tools/asbtool.cpp)
target_link_libraries(asbtool PRIVATE artemis)
target_compile_definitions(asbtool PRIVATE ARTEMIS_TESTDATA="${ARTEMIS_TESTDATA}")
add_executable(asttool tools/asttool.cpp)
target_compile_definitions(asttool PRIVATE ARTEMIS_TESTDATA="${ARTEMIS_TESTDATA}")
target_link_libraries(asttool PRIVATE iconv)
target_link_libraries(asttool PRIVATE artemis iconv)

add_executable(pfsgame tools/pfsgame.cpp)
target_link_libraries(pfsgame PRIVATE artemis)

add_executable(arun tools/arun.cpp)
if(TARGET artemis_render)
	target_link_libraries(arun PRIVATE artemis_render)
endif()
target_link_libraries(arun PRIVATE artemis)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
add_executable(agame tools/agame.cpp)
if(TARGET artemis_render)
	target_link_libraries(agame PRIVATE artemis_render)
endif()
target_link_libraries(agame PRIVATE artemis ${SDL2_LIBRARIES}
	"-framework CoreText" "-framework CoreGraphics" "-framework CoreFoundation"
	"-framework ImageIO")
target_include_directories(agame PRIVATE ${SDL2_INCLUDE_DIRS})
target_link_directories(agame PRIVATE ${SDL2_LIBRARY_DIRS})

# agame is the integration front end: the only target that talks to all the
# layers.  Each layer is optional and gated by a compile definition, so a
# half-written layer degrades to "counted and reported" instead of a link error.
if(TARGET artemis_fx)
	target_link_libraries(agame PRIVATE artemis_fx)
	target_compile_definitions(agame PRIVATE ARTEMIS_AGAME_FX=1)
endif()

# The interactive window: SDL lives in the front end only, so this file is
# compiled into agame rather than into the render library.  It sits in a
# subdirectory because the render library globs src/render/*.cpp and does not
# descend into src/render/play/ - a file there would otherwise be pulled into a
# library that knows nothing about SDL's include path.
if(EXISTS "${CMAKE_SOURCE_DIR}/src/render/play/play.cpp")
	target_sources(agame PRIVATE src/render/play/play.cpp)
endif()
if(TARGET artemis_audio)
	target_link_libraries(agame PRIVATE artemis_audio
		"-framework AVFoundation" "-framework AudioToolbox" "-framework CoreAudio")
	target_compile_definitions(agame PRIVATE ARTEMIS_AGAME_AUDIO=1)
endif()
target_link_libraries(arun PRIVATE artemis)
target_link_libraries(pfstool PRIVATE artemis)

# the layer tools, present only once their sources exist
if(TARGET artemis_audio AND EXISTS "${CMAKE_SOURCE_DIR}/tools/audiotool.cpp")
	add_executable(audiotool tools/audiotool.cpp)
	target_link_libraries(audiotool PRIVATE artemis_audio ${SDL2_LIBRARIES})
	target_include_directories(audiotool PRIVATE ${SDL2_INCLUDE_DIRS})
	target_link_directories(audiotool PRIVATE ${SDL2_LIBRARY_DIRS})
endif()
if(TARGET artemis_save AND EXISTS "${CMAKE_SOURCE_DIR}/tools/savetool.cpp")
	add_executable(savetool tools/savetool.cpp)
	target_link_libraries(savetool PRIVATE artemis_save)
	target_compile_definitions(savetool PRIVATE ARTEMIS_TESTDATA="${ARTEMIS_TESTDATA}")
	# The end-to-end check (save in one process, load in another, six pages
	# identical) needs the interpreter.  Without it savetool still builds and the
	# check reports itself as skipped with the reason.
	if(TARGET artemis_render)
		target_link_libraries(savetool PRIVATE artemis_render
			"-framework CoreText" "-framework CoreGraphics" "-framework ImageIO")
		target_compile_definitions(savetool PRIVATE ARTEMIS_SAVE_VM=1)
	endif()
endif()
if(TARGET artemis_fx AND EXISTS "${CMAKE_SOURCE_DIR}/tools/fxtool.cpp")
	add_executable(fxtool tools/fxtool.cpp)
	target_link_libraries(fxtool PRIVATE artemis_fx)
	# fxtool reads real .iet scripts, so it needs the script layer too.  A static
	# library only pulls the objects it needs, so screen.cpp's CoreText and the
	# frameworks behind it stay out of fxtool's link line... except for the
	# frameworks themselves, which agame already carries.
	if(TARGET artemis_render)
		target_link_libraries(fxtool PRIVATE artemis_render
			"-framework CoreText" "-framework CoreGraphics" "-framework CoreFoundation"
			"-framework ImageIO")
	endif()
endif()
