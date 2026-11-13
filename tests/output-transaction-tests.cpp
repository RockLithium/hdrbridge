#include "output_transaction.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#ifndef __EMSCRIPTEN__
#include <thread>
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
namespace fs = std::filesystem;
using hdrbridge::OutputTransaction;
static void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
static void write(const fs::path& p, const char* data) { std::ofstream f(p, std::ios::binary); f << data; require(bool(f), "write failed"); }
static std::string read(const fs::path& p) { std::ifstream f(p, std::ios::binary); require(bool(f), "read failed"); return {std::istreambuf_iterator<char>(f), {}}; }
struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device random;
        for (int i = 0; i < 128; ++i) {
            auto candidate = fs::temp_directory_path() / fs::path(L"hdrbridge-transaction-测试");
            candidate += "-" + std::to_string(random()) + "-" + std::to_string(random());
            std::error_code ec;
            if (fs::create_directory(candidate, ec)) { path = std::move(candidate); return; }
            if (ec && ec != std::errc::file_exists) throw std::system_error(ec);
        }
        throw std::runtime_error("cannot reserve temporary test directory");
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
};
static void run_tests() {
    TempDir dir;
    auto target = dir.path / "output.tif";
    auto other = dir.path / "output.tif.partial-other";
    write(other, "other");
    std::string error;
    fs::path discarded;
    {
        OutputTransaction a(target), b(target);
        require(a.create(error) && b.create(error), "reserve failed");
        require(a.stage_path() != b.stage_path(), "staging paths collide");
        discarded = a.stage_path();
        write(a.stage_path(), "first");
        write(b.stage_path(), "second");
        write(target, "racer");
        require(!a.commit(false, error), "no-replace accepted an existing target");
        require(read(target) == "racer", "no-replace changed target");
        require(fs::exists(discarded), "failed commit removed staging file");
        require(b.commit(true, error), "replace failed");
        require(read(target) == "second", "replacement content wrong");
    }
    require(!fs::exists(discarded) && read(other) == "other", "cleanup removed wrong file or left staging file");
#ifdef _WIN32
    {
        OutputTransaction a(target);
        require(a.create(error), "reserve failed");
        auto stage = a.stage_path();
        write(stage, "new");
        HANDLE lock = CreateFileW(target.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        require(lock != INVALID_HANDLE_VALUE, "lock failed");
        bool committed = a.commit(true, error);
        CloseHandle(lock);
        require(!committed, "commit replaced locked target");
        require(read(target) == "second", "failed commit changed old bytes");
        require(fs::exists(stage), "failed commit removed staging file");
    }
#endif
    fs::remove(target);
    {
        OutputTransaction a(target), b(target);
        require(a.create(error) && b.create(error), "reserve failed");
        write(a.stage_path(), "first"); write(b.stage_path(), "second");
#ifndef __EMSCRIPTEN__
        bool first = false, second = false;
        std::string first_error, second_error;
        std::thread one([&] { first = a.commit(false, first_error); });
        std::thread two([&] { second = b.commit(false, second_error); });
        one.join(); two.join();
        require(first != second, "concurrent commits did not have exactly one winner");
        require(read(target) == (first ? "first" : "second"), "winner content incorrect");
#else
        require(a.commit(false, error), "first commit failed");
        require(!b.commit(false, error), "second commit unexpectedly succeeded");
        require(read(target) == "first", "winner content incorrect");
#endif
    }
    {
        fs::create_directory(dir.path / "child");
        OutputTransaction a(dir.path / "child" / ".." / "normalized.tif");
        require(a.create(error), "reserve for dot-segment target failed");
        write(a.stage_path(), "normalized path");
        require(a.commit(false, error), "commit for dot-segment target failed");
        require(read(dir.path / "normalized.tif") == "normalized path", "dot-segment output incorrect");
    }
    {
        const auto long_target = dir.path / (std::wstring(210, L'x') + L"-测试.tif");
        OutputTransaction a(long_target);
        require(a.create(error), "reserve for long Unicode name failed");
        write(a.stage_path(), "long name");
        require(a.commit(false, error), "commit for long Unicode name failed");
#ifdef _WIN32
        const fs::path readable(L"\\\\?\\" + long_target.native());
#else
        const auto& readable = long_target;
#endif
        require(read(readable) == "long name", "long Unicode output content wrong");
        require(fs::remove(readable), "long Unicode output cleanup failed");
    }
}
int main() {
    try { run_tests(); }
    catch (const std::exception& e) { std::cerr << "output transaction test: " << e.what() << '\n'; return 1; }
    return 0;
}
