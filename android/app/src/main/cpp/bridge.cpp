#include <jni.h>
#include <atomic>
#include <codecvt>
#include <locale>
#include <mutex>
#include <stdexcept>
#include <string>
#include <nlohmann/json.hpp>
#include "hdrbridge_core.h"
#include "hdrbridge_version.h"

namespace {
std::mutex operation_mutex;
std::atomic_bool cancelled{false};

std::string str(JNIEnv* env, jstring value) {
  if (!value) throw std::invalid_argument("A required string is null");
  const jchar* chars = env->GetStringChars(value, nullptr);
  if (!chars) throw std::runtime_error("Unable to read string");
  std::u16string utf16(reinterpret_cast<const char16_t*>(chars), env->GetStringLength(value));
  env->ReleaseStringChars(value, chars);
  return std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t>{}.to_bytes(utf16);
}
jstring java_string(JNIEnv* env, const std::string& value) {
  const auto utf16 = std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t>{}.from_bytes(value);
  return env->NewString(reinterpret_cast<const jchar*>(utf16.data()), static_cast<jsize>(utf16.size()));
}
void fail(JNIEnv* env, const char* message) noexcept {
  if (env->ExceptionCheck()) return;
  jclass cls = env->FindClass("java/lang/RuntimeException");
  if (!cls) return;
  try {
    jmethodID ctor = env->GetMethodID(cls, "<init>", "(Ljava/lang/String;)V");
    jstring text = ctor ? java_string(env, message) : nullptr;
    if (ctor && text) {
      auto exception = static_cast<jthrowable>(env->NewObject(cls, ctor, text));
      if (exception) { env->Throw(exception); env->DeleteLocalRef(exception); }
    }
    if (text) env->DeleteLocalRef(text);
  } catch (...) {
    if (!env->ExceptionCheck()) env->ThrowNew(cls, "Native operation failed");
  }
  env->DeleteLocalRef(cls);
}
template <typename T> void read(const nlohmann::json& j, const char* key, T& value) {
  if (j.contains(key)) value = j.at(key).get<T>();
}
hdrbridge::ConversionOptions options_from_json(const std::string& input) {
  const auto j = nlohmann::json::parse(input);
  if (!j.is_object()) throw std::invalid_argument("Options must be a JSON object");
  hdrbridge::ConversionOptions o;
#define OPTION(name) read(j, #name, o.name)
  OPTION(mode); OPTION(lossless); OPTION(image_quality); OPTION(effort);
  OPTION(base_quality); OPTION(gainmap_quality); OPTION(gainmap_scale);
  OPTION(multi_channel_gainmap); OPTION(target_peak_nits); OPTION(output_gamut);
  OPTION(output_representation); OPTION(output_transfer); OPTION(embed_hdr_icc);
  OPTION(png_icc_name_override); OPTION(png_compression_level);
  OPTION(tiff_compression_level); OPTION(tiff_compressed); OPTION(diagnostic_icc_only);
  OPTION(diagnostic_icc_profile_path); OPTION(diagnostic_tiff_metadata_source);
  OPTION(diagnostic_windows_compatible_icc); OPTION(copy_exif); OPTION(copy_xmp);
  OPTION(overwrite); OPTION(gainmap_export_format);
  OPTION(frame_format); OPTION(frame_font_path); OPTION(frame_logo_path);
  OPTION(frame_caption1); OPTION(frame_caption2);
#undef OPTION
  return o;
}
struct Callback {
  JavaVM* vm = nullptr;
  jobject ref = nullptr;
  jmethodID method = nullptr;
  Callback(JNIEnv* env, jobject object) {
    if (!object) return;
    env->GetJavaVM(&vm);
    ref = env->NewGlobalRef(object);
    if (!ref) throw std::runtime_error("Unable to retain progress callback");
    jclass cls = env->GetObjectClass(object);
    if (cls) {
      method = env->GetMethodID(cls, "onProgress", "(ILjava/lang/String;)V");
      env->DeleteLocalRef(cls);
    }
    if (!method) { env->DeleteGlobalRef(ref); ref = nullptr; throw std::runtime_error("Invalid progress callback"); }
  }
  ~Callback() {
    if (!ref) return;
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) env->DeleteGlobalRef(ref);
  }
  void call(int percent, const std::string& message) const {
    if (!ref) return;
    JNIEnv* env = nullptr;
    bool attached = false;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
      if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) { cancelled.store(true); return; }
      attached = true;
    }
    jstring text = nullptr;
    try { text = java_string(env, message); } catch (...) { cancelled.store(true); }
    if (text) { env->CallVoidMethod(ref, method, percent, text); env->DeleteLocalRef(text); }
    if (env->ExceptionCheck()) { env->ExceptionClear(); cancelled.store(true); }
    if (attached) vm->DetachCurrentThread();
  }
};
}
extern "C" JNIEXPORT jstring JNICALL Java_org_hdrbridge_NativeBridge_nativeVersion(JNIEnv* env, jclass) {
  return env->NewStringUTF(HDRBRIDGE_VERSION_STRING);
}
extern "C" JNIEXPORT jstring JNICALL Java_org_hdrbridge_NativeBridge_inspect(JNIEnv* env, jclass, jstring path) {
  try { std::lock_guard<std::mutex> lock(operation_mutex); return java_string(env, hdrbridge::to_json(hdrbridge::inspect(str(env, path)))); }
  catch (const std::exception& e) { fail(env, e.what()); return nullptr; }
  catch (...) { fail(env, "Unknown native error"); return nullptr; }
}
extern "C" JNIEXPORT jstring JNICALL Java_org_hdrbridge_NativeBridge_convert(JNIEnv* env, jclass, jstring source, jstring output, jstring json, jobject callback) {
  try {
    std::lock_guard<std::mutex> lock(operation_mutex);
    const auto input = str(env, source), destination = str(env, output);
    const auto options = options_from_json(str(env, json));
    Callback cb(env, callback);
    auto result = hdrbridge::convert(input, destination, options, [&cb](int p, const std::string& m) { cb.call(p, m); }, &cancelled);
    return java_string(env, hdrbridge::to_json(result));
  } catch (const std::exception& e) { fail(env, e.what()); return nullptr; }
  catch (...) { fail(env, "Unknown native error"); return nullptr; }
}
extern "C" JNIEXPORT void JNICALL Java_org_hdrbridge_NativeBridge_resetCancel(JNIEnv*, jclass) { cancelled.store(false); }
extern "C" JNIEXPORT void JNICALL Java_org_hdrbridge_NativeBridge_cancel(JNIEnv*, jclass) { cancelled.store(true); }
extern "C" JNIEXPORT void JNICALL Java_org_hdrbridge_NativeBridge_clearCache(JNIEnv* env, jclass) {
  try { std::lock_guard<std::mutex> lock(operation_mutex); hdrbridge::clear_input_cache(); }
  catch (const std::exception& e) { fail(env, e.what()); }
  catch (...) { fail(env, "Unknown native error"); }
}
