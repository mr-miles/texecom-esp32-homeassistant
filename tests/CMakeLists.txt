# Host-side unit tests for the texecom ESPHome external component.
#
# Tests cover the hardware-independent pieces:
#   * RingBuffer<T, N>         -- tests/test_ring_buffer.cpp
#   * SessionState             -- tests/test_session_state.cpp
#   * PanelModel / Premier 24  -- tests/test_panel_model.cpp
#
# The ESPHome framework headers (esphome/core/*) are NOT pulled in. Only
# the pure C++ headers under components/texecom/ are compiled here.
#
# Build & run:
#     cmake -S tests -B build/tests -Wno-dev
#     cmake --build build/tests
#     ctest --test-dir build/tests --output-on-failure
#
# Requires CMake 3.20+ and a C++17 compiler (gcc / clang / MSVC).

cmake_minimum_required(VERSION 3.20)
project(texecom_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        v3.5.2
)
FetchContent_MakeAvailable(Catch2)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(CTest)
include(Catch)

# Point the tests at the component headers. We also define a sentinel
# macro so ring_buffer.h / panel_model.h / session_state.h are compiled
# in a host context, which skips anything guarded by USE_ARDUINO.
set(TEXECOM_COMPONENT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../components/texecom)

add_executable(texecom_tests
  test_ring_buffer.cpp
  test_session_state.cpp
  test_panel_model.cpp
  ${TEXECOM_COMPONENT_DIR}/panel_model_premier24.cpp
)

target_include_directories(texecom_tests PRIVATE
  ${TEXECOM_COMPONENT_DIR}
)

target_compile_definitions(texecom_tests PRIVATE
  TEXECOM_HOST_TEST=1
)

target_link_libraries(texecom_tests PRIVATE Catch2::Catch2WithMain)

if(MSVC)
  target_compile_options(texecom_tests PRIVATE /W4 /permissive-)
else()
  target_compile_options(texecom_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

catch_discover_tests(texecom_tests)
