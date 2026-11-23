cmake_minimum_required(VERSION 3.25)
project(ctrnative LANGUAGES C CXX)

# A Nintendo 3DS game, recompiled ahead of time by 3dsrecomp, run natively on
# our own kernel and services, drawn by picanative. No emulator.
#   RECOMP_DIR: the game's 3dsrecomp output (recomp.h; the module is built from it)
#   PICANATIVE_DIR: picanative's repository, built (its own README)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(RECOMP_DIR "" CACHE PATH "3dsrecomp's output for the game (recomp.h)")
set(PICANATIVE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../picanative" CACHE PATH "picanative's repository")
if (NOT EXISTS "${RECOMP_DIR}/recomp.h")
  message(FATAL_ERROR "RECOMP_DIR must hold 3dsrecomp's recomp.h (3dsrecomp generate <rom> <dir>)")
endif ()

set(HOST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/host")
set(HOST_SOURCES main.cpp input.cpp rom.cpp kernel.cpp memory.cpp module.cpp services.cpp gsp.cpp dsp.cpp native.cpp)
list(TRANSFORM HOST_SOURCES PREPEND "${HOST_DIR}/")
if (ANDROID)
  # libmain.so, which SDL's activity loads; the engine and SDL3 from picanative's Android build
  set(PICANATIVE_ANDROID_BUILD "${PICANATIVE_DIR}/build-android" CACHE PATH "picanative's Android build")
  set(SDL3_INCLUDE "" CACHE PATH "SDL3's include directory")
  add_library(ctrnative SHARED ${HOST_SOURCES} "${HOST_DIR}/android.cpp")
  set_target_properties(ctrnative PROPERTIES OUTPUT_NAME main)
  # SDL_main (android.cpp) sets the app's folder up, then calls main.cpp's main
  set_source_files_properties("${HOST_DIR}/main.cpp" PROPERTIES COMPILE_DEFINITIONS main=host_main)
  target_include_directories(ctrnative PRIVATE "${SDL3_INCLUDE}")
  target_link_libraries(ctrnative PRIVATE "${PICANATIVE_ANDROID_BUILD}/libpicanative_engine.so"
                        "${PICANATIVE_ANDROID_BUILD}/SDL3/libSDL3.so" dl log android)
else ()
  add_executable(ctrnative ${HOST_SOURCES})
  # the engine's C interface, its DLLs next to the host
  target_link_libraries(ctrnative PRIVATE "${PICANATIVE_DIR}/build/Release/picanative_engine.lib")
  foreach (dll picanative_engine webgpu_dawn dxcompiler dxil SDL3)
    add_custom_command(TARGET ctrnative POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PICANATIVE_DIR}/build/Release/${dll}.dll" "$<TARGET_FILE_DIR:ctrnative>")
  endforeach ()
endif ()
target_include_directories(ctrnative PRIVATE "${RECOMP_DIR}" "${PICANATIVE_DIR}/include")
if (MSVC)
  target_compile_options(ctrnative PRIVATE /W4 /permissive- /utf-8)
endif ()
