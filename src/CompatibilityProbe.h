#pragma once

#include "Civ6FixCore.h"
#include <optional>

namespace civ6fix {

struct CodeEvidence {
    std::uintptr_t rva = 0;
    std::vector<std::uint8_t> bytes;
};

struct CompatibilityLayout {
    std::uint32_t timestamp = 0;
    std::uint32_t imageSize = 0;
    std::uintptr_t imageBase = 0;
    std::uint16_t dllCharacteristics = 0;
    std::uintptr_t unlockIat = 0;
    std::uintptr_t unlockReturn = 0;
    std::uintptr_t lockVector = 0;
    std::uintptr_t discovery = 0;
    std::uintptr_t sso = 0;
    std::vector<CodeEvidence> code;
};

// Read-only, bounded structural recognition. No external configuration or writes.
std::optional<CompatibilityLayout> ProbeCompatibility(
    const std::vector<std::uint8_t>& file);

bool MatchesMutexRuntime(const std::array<std::uint8_t, 34>& code) noexcept;

}  // namespace civ6fix
