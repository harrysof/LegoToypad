cmake_minimum_required(VERSION 3.20)

project(LegoToypad LANGUAGES CXX)

set(GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")

find_program(PYTHON_EXECUTABLE NAMES python python3 py)
if(NOT PYTHON_EXECUTABLE)
  message(FATAL_ERROR "Python is required to regenerate the embedded asset resources (generate_assets.py).")
endif()

# Regenerates resources.rc / GeneratedAssetTable.{h,cpp} / app.ico from the
# "All Bin Files" + "Assets" source folders on every build. The script only
# rewrites a file when its content actually changed, so incremental builds
# don't churn (rc.exe only recompiles when the .rc bytes differ).
add_custom_target(gen_assets ALL
  COMMAND "${PYTHON_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/generate_assets.py"
          --root "${CMAKE_SOURCE_DIR}" --out "${GENERATED_DIR}"
  BYPRODUCTS
    "${GENERATED_DIR}/resources.rc"
    "${GENERATED_DIR}/GeneratedAssetTable.h"
    "${GENERATED_DIR}/GeneratedAssetTable.cpp"
    "${GENERATED_DIR}/app.ico"
  COMMENT "Generating embedded asset resources"
  VERBATIM
)

# SDL2 backs the game-controller input (Xbox / DualShock 4 / DualSense /
# Switch Pro) via its GameController API. Fetched from the official release
# tarball so the version and integrity are pinned, and linked STATICALLY:
# shipping SDL2 as a separate DLL was tripping antivirus heuristics, and the
# static build embeds everything in the exe with no companion DLL to lose.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
include(FetchContent)
FetchContent_Declare(SDL
  URL https://github.com/libsdl-org/SDL/releases/download/release-2.30.10/SDL2-2.30.10.tar.gz
  URL_HASH SHA256=F59ADF36A0FCF4C94198E7D3D776C1B3824211AB7AEEBEB31FE19836661196AA
)
FetchContent_MakeAvailable(SDL)

add_executable(LegoToypad WIN32
  main.cpp
  "${GENERATED_DIR}/GeneratedAssetTable.cpp"
  "${GENERATED_DIR}/resources.rc"
)

add_dependencies(LegoToypad gen_assets)

target_compile_features(LegoToypad PRIVATE cxx_std_20)
# WIN32_LEAN_AND_MEAN is intentionally absent (GDI+ headers need IStream/COM);
# NOMINMAX keeps windows.h's min/max macros from clobbering std::min/std::max.
target_compile_definitions(LegoToypad PRIVATE UNICODE _UNICODE NOMINMAX)
target_include_directories(LegoToypad PRIVATE "${GENERATED_DIR}")
# SDL2 is linked statically and WITHOUT SDL2main: wWinMain stays the entry
# point and SDL2 is used purely as a controller-input library, not taking
# over main.
target_link_libraries(LegoToypad PRIVATE ws2_32 shell32 gdiplus winmm SDL2::SDL2-static)

add_custom_command(TARGET LegoToypad POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${CMAKE_SOURCE_DIR}/README.txt"
          "$<TARGET_FILE_DIR:LegoToypad>/README.txt"
  COMMENT "Copying runtime README"
  VERBATIM
)
