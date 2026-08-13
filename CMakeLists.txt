cmake_minimum_required(VERSION 3.25)
set(VCPKG_OVERLAY_PORTS "${CMAKE_CURRENT_LIST_DIR}/ports-overlay" CACHE STRING "HDR Bridge vcpkg overlay ports")
project(hdrbridge VERSION 1.1.1 LANGUAGES CXX)
include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

find_package(libheif CONFIG REQUIRED)
find_package(libavif CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(PNG REQUIRED)
find_package(TIFF REQUIRED)
find_package(lcms2 CONFIG REQUIRED)
find_path(JXL_INCLUDE_DIR jxl/encode.h REQUIRED)
find_library(JXL_LIBRARY NAMES jxl REQUIRED)
find_library(JXL_THREADS_LIBRARY NAMES jxl_threads REQUIRED)

set(UHDR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(UHDR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(UHDR_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
set(UHDR_BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
set(UHDR_BUILD_DEPS OFF CACHE BOOL "" FORCE)
set(UHDR_ENABLE_HEIF OFF CACHE BOOL "" FORCE)
set(UHDR_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(UHDR_ENABLE_GLES OFF CACHE BOOL "" FORCE)
set(UHDR_ENABLE_INTRINSICS ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_FOR_WINUI ON CACHE BOOL "" FORCE)
if(EXISTS "${CMAKE_SOURCE_DIR}/.tools/libultrahdr/CMakeLists.txt")
  add_subdirectory(.tools/libultrahdr EXCLUDE_FROM_ALL)
else()
  include(FetchContent)
  FetchContent_Declare(libultrahdr
    GIT_REPOSITORY https://github.com/google/libultrahdr.git
    GIT_TAG b2aacb366e1542cfc29605cb0d8a0ebd06bb07f8
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(libultrahdr)
endif()

add_library(hdrbridge_core STATIC
  core/src/hdrbridge_core.cpp
  core/src/hdr_transfer.cpp
  core/src/jpeg_probe.cpp
  core/src/gainmap_adapters.cpp
  core/src/orientation.cpp
)
target_include_directories(hdrbridge_core PUBLIC core/include PRIVATE "${JXL_INCLUDE_DIR}")
target_compile_definitions(hdrbridge_core PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_compile_options(hdrbridge_core PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(hdrbridge_core PUBLIC
  heif
  avif
  "${JXL_LIBRARY}"
  "${JXL_THREADS_LIBRARY}"
  uhdr-static
  nlohmann_json::nlohmann_json
  PNG::PNG
  TIFF::TIFF
  lcms2::lcms2
  windowscodecs ole32 propsys shell32 shlwapi
)

add_executable(hdrbridge-cli core/src/cli.cpp)
target_compile_options(hdrbridge-cli PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(hdrbridge-cli PRIVATE hdrbridge_core)

add_executable(hdrbridge WIN32 desktop/src/main.cpp desktop/src/output_naming.cpp desktop/app.rc)
target_compile_definitions(hdrbridge PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_compile_options(hdrbridge PRIVATE /W4 /permissive- /utf-8)
target_link_options(hdrbridge PRIVATE /MANIFEST:NO)
target_link_libraries(hdrbridge PRIVATE hdrbridge_core comctl32 comdlg32 shell32 dwmapi uxtheme)

install(TARGETS hdrbridge RUNTIME DESTINATION .)

if(BUILD_TESTING)
  add_executable(hdrbridge-orientation-tests tests/orientation-tests.cpp core/src/orientation.cpp)
  target_include_directories(hdrbridge-orientation-tests PRIVATE core/src)
  target_compile_definitions(hdrbridge-orientation-tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(hdrbridge-orientation-tests PRIVATE /W4 /permissive- /utf-8)
  target_link_libraries(hdrbridge-orientation-tests PRIVATE avif)
  add_test(NAME orientation-1-3-6-8 COMMAND hdrbridge-orientation-tests)
  add_executable(hdrbridge-output-naming-tests tests/output-naming-tests.cpp desktop/src/output_naming.cpp)
  target_include_directories(hdrbridge-output-naming-tests PRIVATE desktop/src)
  target_compile_options(hdrbridge-output-naming-tests PRIVATE /W4 /permissive- /utf-8)
  add_test(NAME output-naming COMMAND hdrbridge-output-naming-tests)
  add_executable(hdrbridge-transfer-tests
    tests/hdr-transfer-tests.cpp
    core/src/hdr_transfer.cpp)
  target_include_directories(hdrbridge-transfer-tests PRIVATE core/src)
  target_compile_options(hdrbridge-transfer-tests PRIVATE /W4 /permissive- /utf-8)
  add_test(NAME pq-hlg-numeric COMMAND hdrbridge-transfer-tests)
endif()
