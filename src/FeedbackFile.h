#pragma once
#include "FixSession.h"
#include <filesystem>

namespace civ6fix {
std::filesystem::path FeedbackDirectory();
std::filesystem::path LastFeedbackFile(const std::filesystem::path& directory);
// Only terminal worker snapshots can become shareable files.
std::filesystem::path SaveFeedbackFile(const std::filesystem::path& directory,
    std::wstring_view version, const SessionStatus& status, bool steamRequested);
} // namespace civ6fix
