#include "infrastructure/source_revision.h"
#include <algorithm>

#ifdef _WIN32
#include <Windows.h>
#elif defined(__ANDROID__)
#include <sys/stat.h>
#endif

namespace hdrbridge::detail {

SourceRevision source_revision(const std::filesystem::path& path) {
  SourceRevision result;
#ifdef _WIN32
  const HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return result;
  FILE_BASIC_INFO basic{};
  FILE_ID_INFO id{};
  if (GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic)) &&
      GetFileInformationByHandleEx(file, FileIdInfo, &id, sizeof(id)) &&
      basic.ChangeTime.QuadPart != 0) {
    result.volume = id.VolumeSerialNumber;
    std::copy(std::begin(id.FileId.Identifier), std::end(id.FileId.Identifier), result.file_id.begin());
    result.change_time = basic.ChangeTime.QuadPart;
    result.valid = true;
  }
  CloseHandle(file);
#elif defined(__EMSCRIPTEN__)
  // The Web API compares staged bytes and explicitly invalidates replacements.
  (void)path;
  result.valid = true;
#elif defined(__ANDROID__)
  struct stat info{};
  if (stat(path.c_str(), &info) == 0) {
    result.volume = static_cast<uint64_t>(info.st_dev);
    const auto inode = static_cast<uint64_t>(info.st_ino);
    std::memcpy(result.file_id.data(), &inode, sizeof(inode));
    result.change_time = static_cast<int64_t>(info.st_ctim.tv_sec) * 1000000000ll +
                         info.st_ctim.tv_nsec;
    result.valid = result.change_time != 0;
  }
#else
  // Reuse requires a reliable revision token on other native platforms too.
  (void)path;
#endif
  return result;
}

}  // namespace hdrbridge::detail
