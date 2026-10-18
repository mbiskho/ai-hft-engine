cmake_minimum_required(VERSION 3.20)
project(hft_engine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(HFT_WITH_ONNX "Link ONNX Runtime and enable onnx: models" OFF)
option(HFT_NATIVE "Tune for the build machine (-march=native)" ON)

add_library(hft INTERFACE)
target_include_directories(hft INTERFACE include)
target_compile_options(hft INTERFACE -Wall -Wextra -Wpedantic -Wshadow)
find_package(Threads REQUIRED)
target_link_libraries(hft INTERFACE Threads::Threads)

if(HFT_NATIVE AND CMAKE_BUILD_TYPE STREQUAL "Release")
  target_compile_options(hft INTERFACE -march=native)
endif()

if(HFT_WITH_ONNX)
  find_path(ORT_INCLUDE onnxruntime_cxx_api.h PATH_SUFFIXES onnxruntime REQUIRED)
  find_library(ORT_LIB onnxruntime REQUIRED)
  target_include_directories(hft INTERFACE ${ORT_INCLUDE})
  target_link_libraries(hft INTERFACE ${ORT_LIB})
  target_compile_definitions(hft INTERFACE HFT_WITH_ONNX)
endif()

add_executable(hft-engine src/main.cpp)
target_link_libraries(hft-engine PRIVATE hft)

add_executable(hft-bench bench/bench_hot_path.cpp)
target_link_libraries(hft-bench PRIVATE hft)

enable_testing()
add_executable(hft-tests tests/test_main.cpp)
target_link_libraries(hft-tests PRIVATE hft)
target_compile_definitions(hft-tests PRIVATE HFT_SHARED_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../shared")
add_test(NAME unit COMMAND hft-tests)
