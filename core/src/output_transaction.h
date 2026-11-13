#pragma once

#include <filesystem>
#include <string>

namespace hdrbridge {

// Owns a uniquely reserved sibling of the final output until it is committed.
class OutputTransaction {
public:
    explicit OutputTransaction(std::filesystem::path target);
    ~OutputTransaction();
    OutputTransaction(const OutputTransaction&) = delete;
    OutputTransaction& operator=(const OutputTransaction&) = delete;
    bool create(std::string& error);
    const std::filesystem::path& stage_path() const { return stage_; }
    bool commit(bool overwrite, std::string& error);
private:
    std::filesystem::path target_;
    std::filesystem::path stage_;
};

} // namespace hdrbridge
