# The same implementation is compiled into the desktop, JNI and WASM targets.
function(hdrbridge_add_core_sources target)
  set(core_src "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../core/src")
  target_include_directories(${target} PRIVATE "${core_src}")
  target_sources(${target} PRIVATE
    "${core_src}/canonical/cache.cpp"
    "${core_src}/canonical/input.cpp"
    "${core_src}/codecs/avif.cpp"
    "${core_src}/codecs/gainmap/apple_heif.cpp"
    "${core_src}/codecs/gainmap/avif.cpp"
    "${core_src}/codecs/gainmap/bmff_items.cpp"
    "${core_src}/codecs/gainmap/decode_support.cpp"
    "${core_src}/codecs/gainmap/extract.cpp"
    "${core_src}/codecs/gainmap/extract_support.cpp"
    "${core_src}/codecs/gainmap/icc_description.cpp"
    "${core_src}/codecs/gainmap/iso_metadata.cpp"
    "${core_src}/codecs/gainmap/jpeg.cpp"
    "${core_src}/codecs/gainmap/jpeg_export.cpp"
    "${core_src}/codecs/gainmap/jxl.cpp"
    "${core_src}/codecs/gainmap/tiff.cpp"
    "${core_src}/codecs/heif.cpp"
    "${core_src}/codecs/hsp.cpp"
    "${core_src}/codecs/jpeg_probe.cpp"
    "${core_src}/codecs/jxl.cpp"
    "${core_src}/codecs/jxr.cpp"
    "${core_src}/codecs/png.cpp"
    "${core_src}/codecs/tiff.cpp"
    "${core_src}/codecs/uhdr_helpers.cpp"
    "${core_src}/codecs/ultrahdr.cpp"
    "${core_src}/color/gamut.cpp"
    "${core_src}/color/hdr_transfer.cpp"
    "${core_src}/color/icc_profile.cpp"
    "${core_src}/color/pq.cpp"
    "${core_src}/color/scrgb.cpp"
    "${core_src}/color/signaling.cpp"
    "${core_src}/diagnostics/digest.cpp"
    "${core_src}/diagnostics/result_json.cpp"
    "${core_src}/image/alpha.cpp"
    "${core_src}/image/hdr_stats.cpp"
    "${core_src}/image/orientation.cpp"
    "${core_src}/image/resize.cpp"
    "${core_src}/infrastructure/files.cpp"
    "${core_src}/infrastructure/output_transaction.cpp"
    "${core_src}/infrastructure/progress.cpp"
    "${core_src}/infrastructure/source_revision.cpp"
    "${core_src}/metadata/exif_payload.cpp"
    "${core_src}/metadata/source_metadata.cpp"
    "${core_src}/metadata/tiff_input_exif.cpp"
    "${core_src}/operations/convert.cpp"
    "${core_src}/operations/gainmap_extract.cpp"
    "${core_src}/operations/inspect.cpp"
    "${core_src}/operations/preview.cpp"
    "${core_src}/operations/verify.cpp"
  )
  if(WIN32 OR ANDROID)
    target_sources(${target} PRIVATE
      "${core_src}/photo_frame/frame_exif.cpp"
      "${core_src}/photo_frame/frame_xmp.cpp"
      "${core_src}/photo_frame/photo_frame.cpp"
    )
  endif()
  if(WIN32)
    target_sources(${target} PRIVATE "${core_src}/infrastructure/wic.cpp")
  endif()
endfunction()
