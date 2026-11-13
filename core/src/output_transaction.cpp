#include "output_transaction.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <random>
#include <system_error>
#include <utility>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
// MEMFS lacks hard links; execute the check and rename in one JS task.
EM_JS(int, hb_memfs_rename_if_absent, (const char* from, const char* to), {
    var source = UTF8ToString(from), target = UTF8ToString(to);
    try { FS.lookupPath(target, {follow: false}); return 1; }
    catch (e) { if (e.errno !== 44) return 2; }
    try { FS.rename(source, target); return 0; } catch (e) { return 2; }
});
#endif

namespace hdrbridge {
namespace {
std::atomic<unsigned long long> sequence{0};
#ifdef _WIN32
std::wstring extended_path(const std::filesystem::path& path) {
    auto value = path.native();
    if (value.rfind(L"\\\\?\\", 0) == 0) return value;
    if (value.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + value.substr(2);
    return L"\\\\?\\" + value;
}
#endif
std::string system_error_text(int code) {
    return std::error_code(code, std::system_category()).message();
}
}

OutputTransaction::OutputTransaction(std::filesystem::path target) : target_(std::move(target)) {
    if (!target_.empty()) {
        std::error_code ec;
        auto absolute = std::filesystem::absolute(target_, ec);
        if (!ec) {
#ifdef _WIN32
            // Extended Win32 paths require normalized separators and dot segments.
            target_ = absolute.lexically_normal();
#else
            target_ = std::move(absolute);
#endif
        }
    }
}
OutputTransaction::~OutputTransaction() {
    if (!stage_.empty()) {
        std::error_code ignored;
        std::filesystem::remove(stage_, ignored);
    }
}

bool OutputTransaction::create(std::string& error) {
    error.clear();
    if (!stage_.empty() || target_.empty() || target_.filename().empty()) {
        error = "Invalid output transaction";
        return false;
    }
    try {
        std::random_device random;
        for (int attempt = 0; attempt < 128; ++attempt) {
            auto candidate = target_.parent_path() /
                (".hdrbridge-" + std::to_string(random()) + "-" +
                 std::to_string(random()) + "-" + std::to_string(sequence++) + ".partial");
#ifdef _WIN32
            const auto candidate_native = extended_path(candidate);
            HANDLE file = CreateFileW(candidate_native.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                CloseHandle(file);
                stage_ = std::move(candidate);
                return true;
            }
            int code = static_cast<int>(GetLastError());
            if (code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS) continue;
#else
            int file = open(candidate.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
            if (file >= 0) {
                close(file);
                stage_ = std::move(candidate);
                return true;
            }
            int code = errno;
            if (code == EEXIST) continue;
#endif
            error = "Cannot create temporary output: " + system_error_text(code);
            return false;
        }
        error = "Cannot reserve a unique temporary output";
    } catch (const std::exception& e) {
        error = std::string("Cannot create temporary output: ") + e.what();
    }
    return false;
}

bool OutputTransaction::commit(bool overwrite, std::string& error) {
    error.clear();
    if (stage_.empty()) {
        error = "Temporary output was not created";
        return false;
    }
    std::error_code ec;
    auto status = std::filesystem::symlink_status(target_, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) {
        error = "Cannot inspect output: " + ec.message();
        return false;
    }
    if (std::filesystem::exists(status) && !std::filesystem::is_regular_file(status)) {
        error = "Output is not a regular file";
        return false;
    }
#ifdef _WIN32
    const auto stage_native = extended_path(stage_);
    const auto target_native = extended_path(target_);
    if (!MoveFileExW(stage_native.c_str(), target_native.c_str(), overwrite ? MOVEFILE_REPLACE_EXISTING : 0)) {
        error = "Cannot commit output: " + system_error_text(static_cast<int>(GetLastError()));
        return false;
    }
#else
    if (overwrite) {
        if (rename(stage_.c_str(), target_.c_str()) != 0) {
            error = "Cannot commit output: " + system_error_text(errno);
            return false;
        }
    } else {
#ifdef __EMSCRIPTEN__
        int result = hb_memfs_rename_if_absent(stage_.c_str(), target_.c_str());
        if (result != 0) {
            error = result == 1 ? "Output already exists" : "Cannot commit output";
            return false;
        }
#else
        // link creates the destination only if absent, without a check-then-rename race.
        if (link(stage_.c_str(), target_.c_str()) != 0) {
            error = "Cannot commit output: " + system_error_text(errno);
            return false;
        }
        // The output already exists if unlink fails; leave cleanup to the destructor.
        if (unlink(stage_.c_str()) != 0) return true;
#endif
    }
#endif
    stage_.clear();
    return true;
}
} // namespace hdrbridge
