#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "SteamDiscovery.h"

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <utility>

namespace civ6fix {
namespace {

enum class TokenKind {
    Text,
    OpenBrace,
    CloseBrace,
};

struct Token {
    TokenKind kind;
    std::string text;
};

std::optional<std::vector<Token>> TokenizeVdf(std::string_view input) {
    if (input.size() > kMaxSteamMetadataBytes) {
        return std::nullopt;
    }
    std::vector<Token> tokens;
    const auto appendToken = [&](Token token) {
        if (tokens.size() >= kMaxSteamTokenCount) {
            return false;
        }
        tokens.push_back(std::move(token));
        return true;
    };
    std::size_t index = 0;
    while (index < input.size()) {
        const unsigned char current = static_cast<unsigned char>(input[index]);
        if (std::isspace(current) != 0) {
            ++index;
            continue;
        }
        if (input[index] == '/' && index + 1 < input.size() &&
            input[index + 1] == '/') {
            index += 2;
            while (index < input.size() && input[index] != '\n') {
                ++index;
            }
            continue;
        }
        if (input[index] == '{') {
            if (!appendToken({TokenKind::OpenBrace, {}})) {
                return std::nullopt;
            }
            ++index;
            continue;
        }
        if (input[index] == '}') {
            if (!appendToken({TokenKind::CloseBrace, {}})) {
                return std::nullopt;
            }
            ++index;
            continue;
        }
        if (input[index] != '"') {
            while (index < input.size() &&
                   std::isspace(static_cast<unsigned char>(input[index])) == 0 &&
                   input[index] != '{' && input[index] != '}') {
                ++index;
            }
            continue;
        }

        ++index;
        std::string value;
        bool closed = false;
        while (index < input.size()) {
            const char ch = input[index++];
            if (ch == '"') {
                closed = true;
                break;
            }
            if (ch == '\\' && index < input.size()) {
                const char escaped = input[index++];
                const std::size_t appendedBytes =
                    (escaped == '\\' || escaped == '"' || escaped == 'n' ||
                     escaped == 'r' || escaped == 't')
                        ? 1U
                        : 2U;
                if (value.size() + appendedBytes > kMaxSteamTokenBytes) {
                    return std::nullopt;
                }
                switch (escaped) {
                case '\\': value.push_back('\\'); break;
                case '"': value.push_back('"'); break;
                case 'n': value.push_back('\n'); break;
                case 'r': value.push_back('\r'); break;
                case 't': value.push_back('\t'); break;
                default:
                    value.push_back('\\');
                    value.push_back(escaped);
                    break;
                }
            } else {
                if (value.size() >= kMaxSteamTokenBytes) {
                    return std::nullopt;
                }
                value.push_back(ch);
            }
        }
        if (!closed || !appendToken({TokenKind::Text, std::move(value)})) {
            return std::nullopt;
        }
    }
    return tokens;
}

bool EqualsAsciiInsensitive(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        const unsigned char leftChar = static_cast<unsigned char>(left[index]);
        const unsigned char rightChar = static_cast<unsigned char>(right[index]);
        if (std::tolower(leftChar) != std::tolower(rightChar)) {
            return false;
        }
    }
    return true;
}

std::wstring Utf8ToWide(std::string_view value) {
    if (value.empty()) {
        return {};
    }
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0);
    if (required <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), required) !=
        required) {
        return {};
    }
    return result;
}

bool LooksLikeAbsoluteWindowsPath(std::string_view value) {
    const bool hasControlByte = std::any_of(
        value.begin(), value.end(),
        [](unsigned char ch) { return ch < 0x20U; });
    const bool deviceNamespace =
        value.size() >= 4 && value[0] == '\\' && value[1] == '\\' &&
        (value[2] == '?' || value[2] == '.') && value[3] == '\\';
    if (value.empty() || value.size() > kMaxSteamTokenBytes || hasControlByte ||
        deviceNamespace) {
        return false;
    }
    // Direct UNC/device paths can turn one metadata entry into an unbounded network
    // wait. Local drive letters (including user-mapped drives validated later) are
    // the supported discovery boundary.
    return value.size() >= 3 &&
           std::isalpha(static_cast<unsigned char>(value[0])) != 0 &&
           value[1] == ':' && (value[2] == '\\' || value[2] == '/');
}

void AddUniquePath(std::vector<std::wstring>& paths, std::string_view utf8Path) {
    std::wstring path = Utf8ToWide(utf8Path);
    if (path.empty()) {
        return;
    }
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) {
        path.pop_back();
    }
    const auto duplicate = std::find_if(
        paths.begin(), paths.end(), [&](const std::wstring& existing) {
            if (existing.size() != path.size()) {
                return false;
            }
            for (std::size_t index = 0; index < path.size(); ++index) {
                if (std::towlower(existing[index]) != std::towlower(path[index])) {
                    return false;
                }
            }
            return true;
        });
    if (duplicate == paths.end()) {
        if (paths.size() < kMaxSteamLibraryCount) {
            paths.push_back(std::move(path));
        }
    }
}

}  // namespace

std::vector<std::wstring> ParseSteamLibraryFolders(std::string_view vdf) {
    const auto parsed = TokenizeVdf(vdf);
    if (!parsed.has_value()) {
        return {};
    }
    const std::vector<Token>& tokens = *parsed;
    std::vector<std::wstring> paths;
    for (std::size_t index = 0; index + 1 < tokens.size(); ++index) {
        if (tokens[index].kind != TokenKind::Text ||
            tokens[index + 1].kind != TokenKind::Text) {
            continue;
        }
        if (EqualsAsciiInsensitive(tokens[index].text, "path") &&
            LooksLikeAbsoluteWindowsPath(tokens[index + 1].text)) {
            AddUniquePath(paths, tokens[index + 1].text);
            ++index;
            continue;
        }
        const bool numericKey = !tokens[index].text.empty() &&
            std::all_of(tokens[index].text.begin(), tokens[index].text.end(),
                        [](unsigned char ch) { return std::isdigit(ch) != 0; });
        if (numericKey && LooksLikeAbsoluteWindowsPath(tokens[index + 1].text)) {
            AddUniquePath(paths, tokens[index + 1].text);
            ++index;
        }
    }
    return paths;
}

std::optional<std::wstring> ParseSteamAppInstallDir(std::string_view manifest) {
    const auto parsed = TokenizeVdf(manifest);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    const std::vector<Token>& tokens = *parsed;
    std::optional<std::string> appId;
    std::optional<std::string> installDir;
    for (std::size_t index = 0; index + 1 < tokens.size(); ++index) {
        if (tokens[index].kind != TokenKind::Text ||
            tokens[index + 1].kind != TokenKind::Text) {
            continue;
        }
        if (EqualsAsciiInsensitive(tokens[index].text, "appid")) {
            if (appId.has_value()) {
                return std::nullopt;
            }
            appId = tokens[index + 1].text;
            ++index;
        } else if (EqualsAsciiInsensitive(tokens[index].text, "installdir")) {
            if (installDir.has_value()) {
                return std::nullopt;
            }
            installDir = tokens[index + 1].text;
            ++index;
        }
    }
    if (!appId.has_value() || *appId != "289070" || !installDir.has_value()) {
        return std::nullopt;
    }
    std::wstring value = Utf8ToWide(*installDir);
    return value.empty() ? std::nullopt
                         : std::optional<std::wstring>(std::move(value));
}

}  // namespace civ6fix
