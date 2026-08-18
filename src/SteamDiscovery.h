#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace civ6fix {

inline constexpr std::size_t kMaxSteamMetadataBytes = 1024U * 1024U;
inline constexpr std::size_t kMaxSteamTokenCount = 65536U;
inline constexpr std::size_t kMaxSteamTokenBytes = 32768U;
inline constexpr std::size_t kMaxSteamLibraryCount = 128U;

std::vector<std::wstring> ParseSteamLibraryFolders(std::string_view vdf);
std::optional<std::wstring> ParseSteamAppInstallDir(std::string_view manifest);

}  // namespace civ6fix
