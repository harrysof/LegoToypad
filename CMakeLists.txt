cmake_minimum_required(VERSION 3.20)

project(ToypadPicker LANGUAGES CXX)

add_executable(ToypadPicker WIN32
  main.cpp
)

target_compile_features(ToypadPicker PRIVATE cxx_std_20)
target_compile_definitions(ToypadPicker PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_link_libraries(ToypadPicker PRIVATE ws2_32 xinput9_1_0 shell32)
