cmake_minimum_required(VERSION 3.25)
set(VCPKG_OVERLAY_PORTS "${CMAKE_CURRENT_LIST_DIR}/ports-overlay" CACHE STRING "HDR Bridge vcpkg overlay ports")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" HDRBRIDGE_VERSION LIMIT_COUNT 1)
project(hdrbridge VERSION ${HDRBRIDGE_VERSION} LANGUAGES CXX)
include(CTest)

configure_file(cmake/hdrbridge_version.h.in generated/hdrbridge_version.h @ONLY)
configure_file(desktop/version.rc.in generated/hdrbridge_version.rc @ONLY)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

find_package(libheif CONFIG REQUIRED)
find_package(libavif CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(PNG REQUIRED)
find_package(JPEG REQUIRED)
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
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/PatchUltraHdr.cmake")
if(EXISTS "${CMAKE_SOURCE_DIR}/.tools/libultrahdr/CMakeLists.txt")
  set(HDRBRIDGE_UHDR_ROOT "${CMAKE_SOURCE_DIR}/.tools/libultrahdr")
  hdrbridge_patch_ultrahdr("${CMAKE_SOURCE_DIR}/.tools/libultrahdr")
  add_subdirectory(.tools/libultrahdr EXCLUDE_FROM_ALL)
else()
  include(FetchContent)
  FetchContent_Declare(libultrahdr
    GIT_REPOSITORY https://github.com/google/libultrahdr.git
    GIT_TAG b2aacb366e1542cfc29605cb0d8a0ebd06bb07f8
    GIT_SHALLOW TRUE
    PATCH_COMMAND "${CMAKE_COMMAND}" "-DHDRBRIDGE_UHDR_SOURCE=<SOURCE_DIR>" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PatchUltraHdr.cmake")
  FetchContent_MakeAvailable(libultrahdr)
  hdrbridge_patch_ultrahdr("${libultrahdr_SOURCE_DIR}")
  set(HDRBRIDGE_UHDR_ROOT "${libultrahdr_SOURCE_DIR}")
endif()

add_library(hdrbridge_core STATIC
  core/src/hdrbridge_core.cpp
  core/src/hdr_transfer.cpp
  core/src/jpeg_probe.cpp
  core/src/gainmap_adapters.cpp
  core/src/orientation.cpp
)
target_include_directories(hdrbridge_core PUBLIC core/include PRIVATE "${JXL_INCLUDE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/generated")
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

add_executable(hdrbridge WIN32
  desktop/src/main.cpp
  desktop/src/output_naming.cpp
  desktop/src/app_update.cpp
  desktop/app.rc
  "${CMAKE_CURRENT_BINARY_DIR}/generated/hdrbridge_version.rc")
target_include_directories(hdrbridge PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(hdrbridge PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_compile_options(hdrbridge PRIVATE /W4 /permissive- /utf-8)
target_link_options(hdrbridge PRIVATE /MANIFEST:NO)
target_link_libraries(hdrbridge PRIVATE hdrbridge_core comctl32 comdlg32 shell32 dwmapi uxtheme d3d11 dxgi d3dcompiler winhttp bcrypt)

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
  add_executable(hdrbridge-png-input-tests tests/png-input-tests.cpp)
  target_compile_options(hdrbridge-png-input-tests PRIVATE /W4 /permissive- /utf-8)
  target_link_libraries(hdrbridge-png-input-tests PRIVATE hdrbridge_core)
  add_test(NAME png-sdr-classification COMMAND hdrbridge-png-input-tests)
  add_executable(hdrbridge-cache-tests tests/cache-tests.cpp)
  target_compile_options(hdrbridge-cache-tests PRIVATE /W4 /permissive- /utf-8)
  target_link_libraries(hdrbridge-cache-tests PRIVATE hdrbridge_core)
  add_test(NAME canonical-input-cache COMMAND hdrbridge-cache-tests)
  add_executable(hdrbridge-update-tests tests/update-tests.cpp desktop/src/app_update.cpp)
  target_compile_definitions(hdrbridge-update-tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
  target_compile_options(hdrbridge-update-tests PRIVATE /W4 /permissive- /utf-8)
  target_link_libraries(hdrbridge-update-tests PRIVATE nlohmann_json::nlohmann_json winhttp bcrypt shell32)
  add_test(NAME update-metadata-and-integrity COMMAND hdrbridge-update-tests)
  add_executable(hdrbridge-uhdr-limits-tests tests/uhdr-limits-tests.cpp)
  target_include_directories(hdrbridge-uhdr-limits-tests PRIVATE
    core/src "${HDRBRIDGE_UHDR_ROOT}" "${HDRBRIDGE_UHDR_ROOT}/lib/include")
  target_compile_definitions(hdrbridge-uhdr-limits-tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(hdrbridge-uhdr-limits-tests PRIVATE /W4 /permissive- /utf-8)
  target_link_libraries(hdrbridge-uhdr-limits-tests PRIVATE uhdr-static JPEG::JPEG)
  add_test(NAME uhdr-dimension-and-size-safety COMMAND hdrbridge-uhdr-limits-tests)
  add_executable(hdrbridge-preview-ui-smoke tests/preview_ui_smoke.cpp)
  target_compile_definitions(hdrbridge-preview-ui-smoke PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(hdrbridge-preview-ui-smoke PRIVATE /W4 /permissive- /utf-8)
  target_link_libraries(hdrbridge-preview-ui-smoke PRIVATE user32)
endif()
