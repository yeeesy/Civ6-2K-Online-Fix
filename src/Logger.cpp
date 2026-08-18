#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "Logger.h"

#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>

namespace civ6fix {
namespace {

std::filesystem::path ExecutableDirectory() {
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                            static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return std::filesystem::current_path();
    }
    return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
}

std::string Timestamp() {
    SYSTEMTIME value{};
    GetLocalTime(&value);
    std::ostringstream stream;
    stream << std::setfill('0') << value.wYear << '-' << std::setw(2) << value.wMonth
           << '-' << std::setw(2) << value.wDay << 'T' << std::setw(2) << value.wHour
           << ':' << std::setw(2) << value.wMinute << ':' << std::setw(2)
           << value.wSecond << '.' << std::setw(3) << value.wMilliseconds;
    return stream.str();
}

std::wstring FilenameTimestamp() {
    SYSTEMTIME value{};
    GetLocalTime(&value);
    std::wostringstream stream;
    stream << std::setfill(L'0') << value.wYear << std::setw(2) << value.wMonth
           << std::setw(2) << value.wDay << L'_' << std::setw(2) << value.wHour
           << std::setw(2) << value.wMinute << std::setw(2) << value.wSecond;
    return stream.str();
}

}  // namespace

std::string Utf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), required,
                            nullptr, nullptr) != required) {
        return {};
    }
    return result;
}

std::string JsonEscape(std::string_view value) {
    std::ostringstream output;
    for (unsigned char ch : value) {
        switch (ch) {
        case '\\': output << "\\\\"; break;
        case '"': output << "\\\""; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (ch < 0x20) {
                output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<int>(ch) << std::dec;
            } else {
                output << static_cast<char>(ch);
            }
        }
    }
    return output.str();
}

std::string HexAddress(std::uintptr_t value) {
    std::ostringstream stream;
    stream << "0x" << std::uppercase << std::hex << value;
    return stream.str();
}

bool Logger::Open() {
    std::error_code error;
    std::filesystem::path logDirectory = ExecutableDirectory() / L"logs";
    std::filesystem::create_directories(logDirectory, error);
    if (error) {
        std::vector<wchar_t> localAppData(32768);
        const DWORD length = GetEnvironmentVariableW(
            L"LOCALAPPDATA", localAppData.data(),
            static_cast<DWORD>(localAppData.size()));
        if (length == 0 || length >= localAppData.size()) {
            return false;
        }
        logDirectory = std::filesystem::path(localAppData.data()) /
                       L"Civ6_2K_Online_Fix" / L"logs";
        error.clear();
        std::filesystem::create_directories(logDirectory, error);
        if (error) {
            return false;
        }
    }

    const std::wstring filename = L"Civ6_2K_Online_Fix_" + FilenameTimestamp() +
                                  L"_" +
                                  std::to_wstring(GetCurrentProcessId()) + L".jsonl";
    path_ = logDirectory / filename;
    file_.open(path_, std::ios::binary | std::ios::out | std::ios::app);
    return file_.is_open();
}

void Logger::Event(std::string_view event, std::string_view fields) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream line;
    line << "{\"timestamp\":\"" << Timestamp() << "\",\"event\":\""
         << JsonEscape(event) << '"';
    if (!fields.empty()) {
        line << ',' << fields;
    }
    line << '}';
    const std::string text = line.str();
    std::cout << text << std::endl;
    if (file_.is_open()) {
        file_ << text << '\n';
        file_.flush();
    }
}

}  // namespace civ6fix
