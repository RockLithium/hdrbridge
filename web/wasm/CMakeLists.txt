cmake_minimum_required(VERSION 3.25)

project(hdrbridge_web_core LANGUAGES C CXX)

if(NOT EMSCRIPTEN)
  message(FATAL_ERROR "Configure this target with Emscripten (emcmake cmake).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(libheif CONFIG REQUIRED)
find_package(libavif CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(PNG REQUIRED)
find_package(TIFF REQUIRED)
find_package(lcms2 CONFIG REQUIRED)

set(HDRBRIDGE_VCPKG_PREFIX "" CACHE PATH "Installed wasm32-emscripten dependency prefix")
find_path(JXL_INCLUDE_DIR jxl/encode.h HINTS "${HDRBRIDGE_VCPKG_PREFIX}/include" REQUIRED)
find_library(JXL_LIBRARY NAMES jxl HINTS "${HDRBRIDGE_VCPKG_PREFIX}/lib" REQUIRED NO_DEFAULT_PATH)
find_library(JXL_CMS_LIBRARY NAMES jxl_cms HINTS "${HDRBRIDGE_VCPKG_PREFIX}/lib" REQUIRED NO_DEFAULT_PATH)
find_library(JXL_THREADS_LIBRARY NAMES jxl_threads HINTS "${HDRBRIDGE_VCPKG_PREFIX}/lib" REQUIRED NO_DEFAULT_PATH)
find_library(HIGHWAY_LIBRARY NAMES hwy HINTS "${HDRBRIDGE_VCPKG_PREFIX}/lib" REQUIRED NO_DEFAULT_PATH)
find_library(BROTLI_ENC_LIBRARY NAMES brotlienc HINTS "${HDRBRIDGE_VCPKG_PREFIX}/lib" REQUIRED NO_DEFAULT_PATH)
find_library(BROTLI_DEC_LIBRARY NAMES brotlidec HINTS "${HDRBRIDGE_VCPKG_PREFIX}/lib" REQUIRED NO_DEFAULT_PATH)
find_library(BROTLI_COMMON_LIBRARY NAMES brotlicommon HINTS "${HDRBRIDGE_VCPKG_PREFIX}/lib" REQUIRED NO_DEFAULT_PATH)
find_library(LZMA_LIBRARY NAMES lzma HINTS "${HDRBRIDGE_VCPKG_PREFIX}/lib" REQUIRED NO_DEFAULT_PATH)
find_path(JXR_INCLUDE_DIR JXRGlue.h HINTS "${HDRBRIDGE_VCPKG_PREFIX}/include/jxrlib" REQUIRED)
find_library(JXR_GLUE_LIBRARY NAMES jxrglue HINTS "${HDRBRIDGE_VCPKG_PREFIX}/lib" REQUIRED NO_DEFAULT_PATH)
find_library(JXR_CODEC_LIBRARY NAMES jpegxr HINTS "${HDRBRIDGE_VCPKG_PREFIX}/lib" REQUIRED NO_DEFAULT_PATH)

set(UHDR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(UHDR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(UHDR_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
set(UHDR_BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
set(UHDR_BUILD_DEPS ON CACHE BOOL "" FORCE)
set(UHDR_ENABLE_HEIF OFF CACHE BOOL "" FORCE)
set(UHDR_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(UHDR_ENABLE_GLES OFF CACHE BOOL "" FORCE)
set(UHDR_ENABLE_INTRINSICS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../.tools/libultrahdr/CMakeLists.txt")
  set(HDRBRIDGE_UHDR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.tools/libultrahdr")
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../.tools/libultrahdr"
                   "${CMAKE_CURRENT_BINARY_DIR}/libultrahdr" EXCLUDE_FROM_ALL)
else()
  include(FetchContent)
  FetchContent_Declare(libultrahdr
    GIT_REPOSITORY https://github.com/google/libultrahdr.git
    GIT_TAG b2aacb366e1542cfc29605cb0d8a0ebd06bb07f8
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(libultrahdr)
  set(HDRBRIDGE_UHDR_ROOT "${libultrahdr_SOURCE_DIR}")
endif()

add_executable(hdrbridge-web
  hdrbridge_web_api.cpp
  ../../core/src/hdrbridge_core.cpp
  ../../core/src/hdr_transfer.cpp
  ../../core/src/jpeg_probe.cpp
  ../../core/src/gainmap_adapters.cpp
  ../../core/src/orientation.cpp)

target_include_directories(hdrbridge-web PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../../core/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../core/src"
  "${JXL_INCLUDE_DIR}"
  "${JXR_INCLUDE_DIR}"
  "${HDRBRIDGE_UHDR_ROOT}")

target_compile_options(hdrbridge-web PRIVATE -O3 -fexceptions)
target_compile_definitions(hdrbridge-web PRIVATE __ANSI__)
target_link_libraries(hdrbridge-web PRIVATE
  heif
  avif
  "${JXL_LIBRARY}"
  "${JXL_CMS_LIBRARY}"
  "${JXL_THREADS_LIBRARY}"
  "${HIGHWAY_LIBRARY}"
  "${BROTLI_ENC_LIBRARY}"
  "${BROTLI_DEC_LIBRARY}"
  "${BROTLI_COMMON_LIBRARY}"
  "${JXR_GLUE_LIBRARY}"
  "${JXR_CODEC_LIBRARY}"
  uhdr
  nlohmann_json::nlohmann_json
  PNG::PNG
  TIFF::TIFF
  "${LZMA_LIBRARY}"
  lcms2::lcms2)

target_link_options(hdrbridge-web PRIVATE
  -O3
  -lembind
  -sMODULARIZE=1
  -sEXPORT_ES6=1
  -sEXPORT_NAME=HDRBridgeCore
  -sENVIRONMENT=web,worker,node
  -sALLOW_MEMORY_GROWTH=1
  -sMAXIMUM_MEMORY=4294967296
  -sSTACK_SIZE=8388608
  -sNO_DISABLE_EXCEPTION_CATCHING
  -sFILESYSTEM=1
  -sASSERTIONS=0
  -sEXPORTED_FUNCTIONS=['_malloc','_free','_hb_inspect_asset','_hb_convert_asset','_hb_output_data','_hb_output_size','_hb_info_json','_hb_last_error','_hb_clear_result']
  -sEXPORTED_RUNTIME_METHODS=['HEAPU8','stringToUTF8','lengthBytesUTF8']
  --no-entry)

set_target_properties(hdrbridge-web PROPERTIES
  OUTPUT_NAME "hdrbridge-core"
  SUFFIX ".mjs"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/dist")
