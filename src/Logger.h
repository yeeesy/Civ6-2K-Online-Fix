#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

namespace civ6fix {

std::string Utf8(std::wstring_view value);
std::string JsonEscape(std::string_view value);
std::string HexAddress(std::uintptr_t value);

class Logger {
public:
    bool Open();
    void Event(std::string_view event, std::string_view fields = {});

    const std::filesystem::path& Path() const noexcept { return path_; }

private:
    std::mutex mutex_;
    std::filesystem::path path_;
    std::ofstream file_;
};

}  // namespace civ6fix
