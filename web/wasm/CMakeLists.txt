cmake_minimum_required(VERSION 3.21)

project(hdrbridge_web_core LANGUAGES CXX)

if(NOT EMSCRIPTEN)
  message(FATAL_ERROR "Configure this target with Emscripten (emcmake cmake).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(HDRBRIDGE_HEIF_ROOT "" CACHE PATH "Installed libheif prefix")
set(HDRBRIDGE_DE265_ROOT "" CACHE PATH "Installed libde265 prefix")

set(HEIF_INCLUDE_DIR "${HDRBRIDGE_HEIF_ROOT}/include")
set(HEIF_LIBRARY "${HDRBRIDGE_HEIF_ROOT}/lib/libheif.a")
set(DE265_LIBRARY "${HDRBRIDGE_DE265_ROOT}/lib/libde265.a")
foreach(required_path IN ITEMS
    "${HEIF_INCLUDE_DIR}/libheif/heif.h"
    "${HEIF_LIBRARY}"
    "${DE265_LIBRARY}")
  if(NOT EXISTS "${required_path}")
    message(FATAL_ERROR "Required Web codec dependency is missing: ${required_path}")
  endif()
endforeach()

add_executable(hdrbridge-web
  hdrbridge_web.cpp
  ../../core/src/hdr_transfer.cpp)

target_include_directories(hdrbridge-web PRIVATE
  "${HEIF_INCLUDE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../core/src")

target_compile_options(hdrbridge-web PRIVATE
  -O3
  -sUSE_LIBPNG=1
  -sUSE_ZLIB=1)

target_link_libraries(hdrbridge-web PRIVATE
  "${HEIF_LIBRARY}"
  "${DE265_LIBRARY}")

target_link_options(hdrbridge-web PRIVATE
  -O3
  -lembind
  -sUSE_LIBPNG=1
  -sUSE_ZLIB=1
  -sMODULARIZE=1
  -sEXPORT_ES6=1
  -sEXPORT_NAME=HDRBridgeCore
  -sENVIRONMENT=web,worker,node
  -sALLOW_MEMORY_GROWTH=1
  -sMAXIMUM_MEMORY=4294967296
  -sFILESYSTEM=0
  -sASSERTIONS=0
  -sEXPORTED_FUNCTIONS=['_malloc','_free','_hb_inspect_heif','_hb_convert_heif_to_png','_hb_png_pixel_crc32','_hb_output_data','_hb_output_size','_hb_info_json','_hb_last_error','_hb_clear_result']
  -sEXPORTED_RUNTIME_METHODS=['HEAPU8']
  --no-entry)

set_target_properties(hdrbridge-web PROPERTIES
  OUTPUT_NAME "hdrbridge-core"
  SUFFIX ".mjs"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/dist")
