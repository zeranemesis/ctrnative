cmake_minimum_required(VERSION 3.25)
project(ctrnative_menu LANGUAGES CXX)

# The HOME-like menu: lists the games of a folder, launches them through tools/ctrn.py.
# A project of its own (no game's RECOMP_DIR needed). SDL3 as picanative finds it:
# CMAKE_PREFIX_PATH or SDL3_DIR. Windows only (text drawn by GDI, games run through a job).
#   cmake -S src/menu -B build-menu -G Ninja -DCMAKE_CXX_COMPILER=clang-cl -DSDL3_DIR=<SDL3>/cmake
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(SDL3 REQUIRED CONFIG)
add_executable(ctrnative_menu WIN32 menu.cpp)
target_link_libraries(ctrnative_menu PRIVATE SDL3::SDL3 gdi32)
# tools/ctrn.py, the launcher the menu runs
get_filename_component(CTRN_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
target_compile_definitions(ctrnative_menu PRIVATE CTRN_ROOT="${CTRN_ROOT}" UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
target_compile_options(ctrnative_menu PRIVATE /W4 /utf-8)
add_custom_command(TARGET ctrnative_menu POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3> "$<TARGET_FILE_DIR:ctrnative_menu>")
