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

add_library(artemis STATIC src/pfs.cpp src/ast.cpp)
target_include_directories(artemis PUBLIC src)
target_link_libraries(artemis PUBLIC "-framework CoreFoundation")

add_executable(pfstool tools/pfstool.cpp)
add_executable(asttool tools/asttool.cpp)
target_link_libraries(asttool PRIVATE iconv)
target_link_libraries(asttool PRIVATE artemis)
target_link_libraries(pfstool PRIVATE artemis)
