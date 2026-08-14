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
target_link_libraries(LegoToypad PRIVATE ws2_32 xinput9_1_0 shell32 gdiplus)