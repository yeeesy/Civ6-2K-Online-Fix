#pragma once

#include <array>
#include <cwctype>
#include <cstdint>
#include <limits>
#include <string_view>
#include <vector>

namespace civ6fix {

enum class InstanceLockState {
    Acquired,
    AlreadyOwned,
    Failed,
};

enum class InstanceStartupDecision {
    ContinueStartup,
    ActivateExistingAndExit,
    FailClosed,
};

constexpr InstanceStartupDecision DecideInstanceStartup(
    InstanceLockState state) noexcept {
    switch (state) {
    case InstanceLockState::Acquired:
        return InstanceStartupDecision::ContinueStartup;
    case InstanceLockState::AlreadyOwned:
        return InstanceStartupDecision::ActivateExistingAndExit;
    case InstanceLockState::Failed:
        return InstanceStartupDecision::FailClosed;
    }
    return InstanceStartupDecision::FailClosed;
}

enum class Renderer {
    DirectX11,
    DirectX12,
};

enum class SupportState {
    Verified,
    Experimental,
    ReadOnlyCandidate,
    Unsupported,
};

struct BuildProfile {
    const wchar_t* id;
    const wchar_t* executableName;
    const wchar_t* sha256;
    Renderer renderer;
    SupportState supportState;
    std::uint64_t fileSize;
    std::uint32_t peTimestamp;
    std::uintptr_t preferredImageBase;
    std::uint32_t imageSize;
    std::uint16_t dllCharacteristics;
    std::uintptr_t mtxUnlockIatRva;
    std::uintptr_t opensslUnlockReturnRva;
    std::uintptr_t opensslLockVectorRva;
    std::uintptr_t discoveryGlobalRva;
    std::uintptr_t ssoGlobalRva;
    std::uint32_t mutexOwnerOffset;
    std::uint32_t mutexCountOffset;
    std::uint32_t opensslLockIndex;
};

inline constexpr BuildProfile kVerifiedDx11Profile{
    L"civ6-win64steam-1.0.12.68-dx11",
    L"CivilizationVI.exe",
    L"E7450823CC8E00468CFF7B9D7B97C63140EAE38AE1D774BA4EFA437556C42D63",
    Renderer::DirectX11,
    SupportState::Verified,
    21160312,
    0x667C6FC6,
    0x140000000,
    0x03704000,
    0x8120,
    0x00F96E70,
    0x00BCEEC5,
    0x03551668,
    0x03551700,
    0x03551598,
    0x48,
    0x4C,
    18,
};

inline constexpr wchar_t kDx12ProfileId[] =
    L"civ6-win64steam-1.0.12.68-dx12";
inline constexpr SupportState kDx12SupportState = SupportState::Verified;

inline constexpr BuildProfile kDx12Profile{
    kDx12ProfileId,
    L"CivilizationVI_DX12.exe",
    L"C2C3D40B86260A541D8A4D38CB70D50D3406AE1DE374AFC382D1E42BC1342F1E",
    Renderer::DirectX12,
    kDx12SupportState,
    21391464,
    0x667C7363,
    0x140000000,
    0x0FE55000,
    0x8120,
    0x00FA5E78,
    0x00BDDA15,
    0x0FCA0FE8,
    0x0FCA1080,
    0x0FCA0F18,
    0x48,
    0x4C,
    18,
};

inline constexpr std::array<const BuildProfile*, 2> kKnownBuildProfiles{
    &kVerifiedDx11Profile,
    &kDx12Profile,
};

inline bool EqualsInsensitive(std::wstring_view left,
                              std::wstring_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (std::towlower(left[index]) != std::towlower(right[index])) {
            return false;
        }
    }
    return true;
}

inline const BuildProfile* FindBuildProfile(std::wstring_view executableName,
                                             std::wstring_view sha256) noexcept {
    for (const BuildProfile* profile : kKnownBuildProfiles) {
        if (EqualsInsensitive(executableName, profile->executableName) &&
            EqualsInsensitive(sha256, profile->sha256)) {
            return profile;
        }
    }
    return nullptr;
}

enum class BuildUseDecision {
    InstallVerifiedGuard,
    ObserveCandidateReadOnly,
    RejectUnknownNoWrite,
};

constexpr BuildUseDecision DecideBuildUse(
    const BuildProfile* profile) noexcept {
    if (profile == nullptr || profile->supportState == SupportState::Unsupported) {
        return BuildUseDecision::RejectUnknownNoWrite;
    }
    if (profile->supportState == SupportState::Verified) {
        return BuildUseDecision::InstallVerifiedGuard;
    }
    return BuildUseDecision::ObserveCandidateReadOnly;
}

struct RuntimeFileIdentityEvidence {
    bool fingerprintMatched;
    bool sizeMatched;
    bool digestMatched;
    bool mainModulePathMatched;
};

struct RuntimeProcessIdentityEvidence {
    bool pidMatched;
    bool creationTimeMatched;
    bool imagePathMatched;
};

enum class RuntimeProcessIdentityDecision {
    AcceptSameProcess,
    RejectPidMismatch,
    RejectCreationTimeMismatch,
    RejectImagePathMismatch,
};

constexpr RuntimeProcessIdentityDecision DecideRuntimeProcessIdentity(
    const RuntimeProcessIdentityEvidence& evidence) noexcept {
    if (!evidence.pidMatched) {
        return RuntimeProcessIdentityDecision::RejectPidMismatch;
    }
    if (!evidence.creationTimeMatched) {
        return RuntimeProcessIdentityDecision::RejectCreationTimeMismatch;
    }
    if (!evidence.imagePathMatched) {
        return RuntimeProcessIdentityDecision::RejectImagePathMismatch;
    }
    return RuntimeProcessIdentityDecision::AcceptSameProcess;
}

enum class RuntimeFileIdentityDecision {
    AcceptExactIdentity,
    RejectFingerprintMismatch,
    RejectSizeMismatch,
    RejectDigestMismatch,
    RejectMainModulePathMismatch,
};

constexpr RuntimeFileIdentityDecision DecideRuntimeFileIdentity(
    const RuntimeFileIdentityEvidence& evidence) noexcept {
    if (!evidence.fingerprintMatched) {
        return RuntimeFileIdentityDecision::RejectFingerprintMismatch;
    }
    if (!evidence.sizeMatched) {
        return RuntimeFileIdentityDecision::RejectSizeMismatch;
    }
    if (!evidence.digestMatched) {
        return RuntimeFileIdentityDecision::RejectDigestMismatch;
    }
    if (!evidence.mainModulePathMatched) {
        return RuntimeFileIdentityDecision::RejectMainModulePathMismatch;
    }
    return RuntimeFileIdentityDecision::AcceptExactIdentity;
}

struct RuntimeLibraryIdentityEvidence {
    bool moduleNameMatched;
    bool systemPathMatched;
    bool exactExportMatched;
    bool executableImageMatched;
};

enum class RuntimeLibraryIdentityDecision {
    AcceptExactIdentity,
    RejectModuleNameMismatch,
    RejectSystemPathMismatch,
    RejectExportMismatch,
    RejectExecutableImageMismatch,
};

constexpr RuntimeLibraryIdentityDecision DecideRuntimeLibraryIdentity(
    const RuntimeLibraryIdentityEvidence& evidence) noexcept {
    if (!evidence.moduleNameMatched) {
        return RuntimeLibraryIdentityDecision::RejectModuleNameMismatch;
    }
    if (!evidence.systemPathMatched) {
        return RuntimeLibraryIdentityDecision::RejectSystemPathMismatch;
    }
    if (!evidence.exactExportMatched) {
        return RuntimeLibraryIdentityDecision::RejectExportMismatch;
    }
    if (!evidence.executableImageMatched) {
        return RuntimeLibraryIdentityDecision::RejectExecutableImageMismatch;
    }
    return RuntimeLibraryIdentityDecision::AcceptExactIdentity;
}

enum class RuntimeIatDecision {
    AcceptExactTarget,
    KeepWaiting,
    RejectMismatch,
};

constexpr RuntimeIatDecision DecideRuntimeIatTarget(
    bool exactTargetObserved, bool waitExpired) noexcept {
    if (exactTargetObserved) {
        return RuntimeIatDecision::AcceptExactTarget;
    }
    return waitExpired ? RuntimeIatDecision::RejectMismatch
                       : RuntimeIatDecision::KeepWaiting;
}

constexpr bool IsRvaSpanWithinImage(std::uint32_t imageSize,
                                    std::uintptr_t rva,
                                    std::size_t span) noexcept {
    return span != 0 && rva < imageSize &&
           span <= static_cast<std::size_t>(imageSize - rva);
}

constexpr bool IsAddressAdditionSafe(std::uintptr_t base,
                                     std::uintptr_t offset) noexcept {
    return base <= (std::numeric_limits<std::uintptr_t>::max)() - offset;
}

constexpr bool IsPointerVectorIndexAvailable(std::uintptr_t begin,
                                             std::uintptr_t end,
                                             std::uint32_t index) noexcept {
    constexpr std::uintptr_t width = sizeof(std::uintptr_t);
    if (begin == 0 || end <= begin || begin % width != 0 || end % width != 0) {
        return false;
    }
    const std::uintptr_t span = end - begin;
    return span % width == 0 &&
           static_cast<std::uintptr_t>(index) < span / width;
}

enum class IatOwnership {
    OwnedBySession,
    OriginalTarget,
    ForeignOwner,
    Unreadable,
};

enum class HookCleanupOutcome {
    RestoreOwnedAndRetainAllocation,
    RestorePointerVerifiedProtectionUncertainRetainAllocation,
    AlreadyOriginalAndRetainAllocation,
    ForeignOwnerNoWrite,
    UnreadableNoWrite,
    TargetExited,
};

struct HookCleanupPlan {
    bool restoreOriginalIat;
    bool freeRemoteAllocation;
    HookCleanupOutcome outcome;
};

constexpr HookCleanupPlan PlanHookCleanup(bool targetAlive,
                                           IatOwnership ownership) noexcept {
    if (!targetAlive) {
        return {false, false, HookCleanupOutcome::TargetExited};
    }
    switch (ownership) {
    case IatOwnership::OwnedBySession:
        return {true, false,
                HookCleanupOutcome::RestoreOwnedAndRetainAllocation};
    case IatOwnership::OriginalTarget:
        return {false, false,
                HookCleanupOutcome::AlreadyOriginalAndRetainAllocation};
    case IatOwnership::ForeignOwner:
        return {false, false, HookCleanupOutcome::ForeignOwnerNoWrite};
    case IatOwnership::Unreadable:
        return {false, false, HookCleanupOutcome::UnreadableNoWrite};
    }
    return {false, false, HookCleanupOutcome::UnreadableNoWrite};
}

struct PointerPatchEvidence {
    bool madeWritable;
    bool writeCompleted;
    bool protectionRestored;
    bool readBackCompleted;
    bool expectedValueObserved;
    bool ownershipMatched = true;
};

enum class PointerPatchOutcome {
    Succeeded,
    MakeWritableFailed,
    WriteFailed,
    ProtectionRestoreFailed,
    OwnershipChanged,
    ReadBackFailed,
    VerificationFailed,
};

constexpr PointerPatchOutcome EvaluatePointerPatch(
    const PointerPatchEvidence& evidence) noexcept {
    if (!evidence.madeWritable) {
        return PointerPatchOutcome::MakeWritableFailed;
    }
    if (!evidence.protectionRestored) {
        return PointerPatchOutcome::ProtectionRestoreFailed;
    }
    if (!evidence.ownershipMatched) {
        return PointerPatchOutcome::OwnershipChanged;
    }
    if (!evidence.writeCompleted) {
        return PointerPatchOutcome::WriteFailed;
    }
    if (!evidence.readBackCompleted) {
        return PointerPatchOutcome::ReadBackFailed;
    }
    return evidence.expectedValueObserved ? PointerPatchOutcome::Succeeded
                                          : PointerPatchOutcome::VerificationFailed;
}

constexpr bool CanFreeGuardAfterFailedPublish(
    const PointerPatchEvidence& evidence, std::uintptr_t observedValue,
    std::uintptr_t originalValue) noexcept {
    if (!evidence.madeWritable) {
        // The pointer write path was never entered.
        return true;
    }
    if (!evidence.ownershipMatched) {
        // The guarded write was never attempted.
        return true;
    }
    return !evidence.writeCompleted && evidence.readBackCompleted &&
           observedValue == originalValue;
}

enum class FailedPublishDisposition {
    NoHookSafeToFree,
    NoHookProtectionUncertainSafeToFree,
    OriginalRestoredRetainAllocation,
    OriginalRestoredProtectionUncertainRetainAllocation,
    StateUncertainRetainAllocation,
};

constexpr FailedPublishDisposition DecideFailedPublishDisposition(
    const PointerPatchEvidence& publishEvidence,
    std::uintptr_t observedAfterPublish, std::uintptr_t originalValue,
    bool restorationAttempted,
    const PointerPatchEvidence& restoreEvidence,
    std::uintptr_t observedAfterRestore) noexcept {
    if (CanFreeGuardAfterFailedPublish(
            publishEvidence, observedAfterPublish, originalValue)) {
        if (publishEvidence.madeWritable &&
            !publishEvidence.protectionRestored) {
            return FailedPublishDisposition::
                NoHookProtectionUncertainSafeToFree;
        }
        return FailedPublishDisposition::NoHookSafeToFree;
    }
    const bool finalReadCompleted =
        restorationAttempted ? restoreEvidence.readBackCompleted
                             : publishEvidence.readBackCompleted;
    const std::uintptr_t finalObserved =
        restorationAttempted ? observedAfterRestore : observedAfterPublish;
    if (!finalReadCompleted || finalObserved != originalValue) {
        return FailedPublishDisposition::StateUncertainRetainAllocation;
    }
    const bool finalProtectionRestored =
        restorationAttempted ? restoreEvidence.protectionRestored
                             : publishEvidence.protectionRestored;
    return finalProtectionRestored
               ? FailedPublishDisposition::OriginalRestoredRetainAllocation
               : FailedPublishDisposition::
                     OriginalRestoredProtectionUncertainRetainAllocation;
}

enum class GamePresence {
    NotRunning,
    Running,
    Unknown,
};

enum class BuildCompatibility {
    Compatible,
    Incompatible,
};

enum class PreflightDecision {
    LaunchAndProtect,
    RefuseExistingGame,
    RefuseProcessScanFailure,
    RejectBuild,
};

constexpr PreflightDecision DecidePreflight(
    GamePresence gamePresence,
    BuildCompatibility buildCompatibility) noexcept {
    if (gamePresence == GamePresence::Running) {
        return PreflightDecision::RefuseExistingGame;
    }
    if (gamePresence == GamePresence::Unknown) {
        return PreflightDecision::RefuseProcessScanFailure;
    }
    return buildCompatibility == BuildCompatibility::Compatible
               ? PreflightDecision::LaunchAndProtect
               : PreflightDecision::RejectBuild;
}

struct MonitorSnapshot {
    std::uint64_t skippedInvalidUnlocks;
    int discoveryState;
    int ssoState;
    bool timedOut;
    bool stopRequested;
};

enum class MonitorDecision {
    ContinueMonitoring,
    DetachAndKeepHook,
    DetachOnlineWithoutIntervention,
    DetachTimedOutAndKeepHook,
    RestoreAndStop,
};

constexpr MonitorDecision DecideMonitoring(const MonitorSnapshot& snapshot) noexcept {
    if (snapshot.stopRequested) {
        return MonitorDecision::RestoreAndStop;
    }
    if (snapshot.skippedInvalidUnlocks > 0 && snapshot.discoveryState >= 4 &&
        snapshot.ssoState >= 4) {
        return MonitorDecision::DetachAndKeepHook;
    }
    if (snapshot.discoveryState >= 4 && snapshot.ssoState >= 4) {
        return MonitorDecision::DetachOnlineWithoutIntervention;
    }
    if (snapshot.timedOut) {
        return MonitorDecision::DetachTimedOutAndKeepHook;
    }
    return MonitorDecision::ContinueMonitoring;
}

constexpr bool ShouldSkipUnlock(std::uintptr_t actualReturnAddress,
                                std::uintptr_t expectedReturnAddress,
                                std::int32_t mutexCount) noexcept {
    return actualReturnAddress == expectedReturnAddress && mutexCount <= 0;
}

inline void Append64(std::vector<std::uint8_t>& code, std::uint64_t value) {
    for (int index = 0; index < 8; ++index) {
        code.push_back(static_cast<std::uint8_t>((value >> (index * 8)) & 0xFF));
    }
}

inline std::vector<std::uint8_t> BuildUnlockGuard(std::uintptr_t expectedReturn,
                                                   std::uintptr_t counter,
                                                   std::uintptr_t originalUnlock,
                                                   std::uint8_t mutexCountOffset =
                                                       0x4C) {
    std::vector<std::uint8_t> code;
    code.reserve(51);
    code.insert(code.end(), {0x48, 0xB8});
    Append64(code, expectedReturn);                    // mov rax, expected return
    code.insert(code.end(), {0x48, 0x39, 0x04, 0x24}); // cmp [rsp], rax
    code.insert(code.end(), {0x75, 0x17});             // jne forward
    code.insert(code.end(), {0x83, 0x79, mutexCountOffset,
                             0x00});                    // cmp dword [rcx+disp8], 0
    code.insert(code.end(), {0x7F, 0x11});             // jg forward
    code.insert(code.end(), {0x48, 0xB8});
    Append64(code, counter);                           // mov rax, counter
    code.insert(code.end(), {0xF0, 0x48, 0xFF, 0x00}); // lock inc qword [rax]
    code.insert(code.end(), {0x31, 0xC0, 0xC3});       // xor eax,eax; ret
    code.insert(code.end(), {0x48, 0xB8});
    Append64(code, originalUnlock);                    // mov rax, original
    code.insert(code.end(), {0xFF, 0xE0});             // jmp rax
    return code;
}

}  // namespace civ6fix
