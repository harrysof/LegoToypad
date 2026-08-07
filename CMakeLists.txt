cmake_minimum_required(VERSION 3.20)

project(LegoToypad LANGUAGES CXX)

add_executable(LegoToypad WIN32
  main.cpp
)

target_compile_features(LegoToypad PRIVATE cxx_std_20)
target_compile_definitions(LegoToypad PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_link_libraries(LegoToypad PRIVATE ws2_32 xinput9_1_0 shell32)
