#pragma once

#include "Civ6FixCore.h"

#include <atomic>
#include <cstdint>
#include <string>

namespace civ6fix {

enum class SessionPhase {
    Created,
    Preparing,
    Listening,
    WaitingForGame,
    ValidatingRuntime,
    InstallingGuard,
    Monitoring,
    Completed,
};

enum class SessionResult {
    None,
    Fixed,
    OnlineWithoutIntervention,
    MonitorTimedOutHookKept,
    CandidateDetectedReadOnly,
    TargetExited,
    CancelledBeforeWrite,
    CancelledOwnedHookRestoredAllocationRetained,
    CancelledOwnedHookRestoredProtectionUncertainAllocationRetained,
    CancelledHookNotOwnedNoWrite,
    ExistingGameNoWrite,
    ProcessScanFailedNoWrite,
    InstallationNotFoundNoWrite,
    UnsupportedBuildNoWrite,
    HashFailedNoWrite,
    SteamLaunchFailedNoWrite,
    TargetWaitTimedOutNoWrite,
    RuntimeMismatchNoWrite,
    GuardInstallFailed,
    GuardInstallFailedNoHookProtectionUncertain,
    GuardInstallFailedRestoredAllocationRetained,
    GuardInstallFailedRestoredProtectionUncertainAllocationRetained,
    GuardInstallFailedStateUncertainAllocationRetained,
    TargetWaitFailedHookKept,
};

constexpr int ExitCodeForSessionResult(SessionResult result) noexcept {
    switch (result) {
    case SessionResult::Fixed:
    case SessionResult::OnlineWithoutIntervention:
        return 0;
    case SessionResult::CandidateDetectedReadOnly:
        return 5;
    case SessionResult::MonitorTimedOutHookKept:
        return 30;
    case SessionResult::CancelledBeforeWrite:
    case SessionResult::CancelledOwnedHookRestoredAllocationRetained:
    case SessionResult::
        CancelledOwnedHookRestoredProtectionUncertainAllocationRetained:
        return 31;
    case SessionResult::TargetExited:
        return 32;
    default:
        return 10;
    }
}

constexpr int ExitCodeForExistingInstance(bool headless) noexcept {
    return headless ? 8 : 0;
}

struct SessionStatus {
    SessionPhase phase = SessionPhase::Created;
    SessionResult result = SessionResult::None;
    const BuildProfile* profile = nullptr;
    std::uint32_t pid = 0;
    std::uint64_t skippedInvalidUnlocks = 0;
    int discoveryState = -1;
    int ssoState = -1;
    std::wstring detail;
};

class ISessionObserver {
public:
    virtual ~ISessionObserver() = default;
    virtual void OnSessionStatus(const SessionStatus& status) = 0;
};

enum class PreparationOutcome {
    Ready,
    ExistingGame,
    ProcessScanFailed,
    InstallationNotFound,
    UnsupportedBuild,
    HashFailed,
};

struct PreparationResult {
    PreparationOutcome outcome = PreparationOutcome::InstallationNotFound;
    std::wstring detail;
};

struct TargetProcess {
    std::uint32_t pid = 0;
    const BuildProfile* profile = nullptr;
    std::wstring imagePath;
    std::uint64_t creationTime = 0;
};

enum class TargetScanOutcome {
    NotFound,
    Found,
    Failed,
};

struct TargetScanResult {
    TargetScanOutcome outcome = TargetScanOutcome::NotFound;
    TargetProcess target;
    std::uint32_t win32Error = 0;
};

enum class RuntimeValidationOutcome {
    Valid,
    ProcessExited,
    ProcessIdentityMismatch,
    ImagePathMismatch,
    PeIdentityMismatch,
    IatTargetMismatch,
    MutexLayoutMismatch,
    ReadFailed,
};

struct RuntimeValidationResult {
    RuntimeValidationOutcome outcome = RuntimeValidationOutcome::ReadFailed;
    std::uint32_t win32Error = 0;
    std::wstring detail;
};

struct InstalledGuard {
    std::uintptr_t iatAddress = 0;
    std::uintptr_t originalTarget = 0;
    std::uintptr_t guardAddress = 0;
    std::uintptr_t allocationAddress = 0;
    std::uintptr_t counterAddress = 0;
};

enum class GuardInstallOutcome {
    Installed,
    FailedNoHook,
    FailedNoHookProtectionUncertain,
    FailedRestoredAllocationRetained,
    FailedRestoredProtectionUncertainAllocationRetained,
    FailedStateUncertainAllocationRetained,
};

struct GuardInstallResult {
    GuardInstallOutcome outcome = GuardInstallOutcome::FailedNoHook;
    InstalledGuard guard;
    std::uint32_t win32Error = 0;
    std::wstring detail;
};

enum class TargetRunState {
    Running,
    Exited,
    WaitFailed,
};

struct MonitorSample {
    bool readable = false;
    std::uint64_t skippedInvalidUnlocks = 0;
    int discoveryState = -1;
    int ssoState = -1;
};

class IFixSessionPlatform {
public:
    virtual ~IFixSessionPlatform() = default;

    virtual PreparationResult Prepare() = 0;
    virtual bool LaunchViaSteam(std::wstring& detail) = 0;
    virtual TargetScanResult ScanForTarget() = 0;
    virtual RuntimeValidationResult ValidateRuntime(
        const TargetProcess& target, const BuildProfile& profile) = 0;
    virtual GuardInstallResult InstallGuard(const TargetProcess& target,
                                             const BuildProfile& profile) = 0;
    virtual TargetRunState QueryTargetState(const TargetProcess& target) = 0;
    virtual MonitorSample ReadMonitorSample(const TargetProcess& target,
                                             const InstalledGuard& guard) = 0;
    virtual HookCleanupOutcome RestoreOwnedHook(
        const TargetProcess& target, const InstalledGuard& guard) = 0;
    virtual std::uint64_t MonotonicMilliseconds() = 0;
    virtual void SleepFor(std::uint32_t milliseconds) = 0;
};

struct SessionOptions {
    bool launchViaSteam = false;
    std::uint32_t targetWaitTimeoutMs = 5U * 60U * 1000U;
    std::uint32_t targetPollMs = 25;
    std::uint32_t monitorTimeoutMs = 90U * 1000U;
    std::uint32_t monitorPollMs = 10;
};

class FixSession {
public:
    FixSession(IFixSessionPlatform& platform, ISessionObserver& observer,
               SessionOptions options = {});

    SessionResult Run();
    void RequestStop() noexcept;
    void RequestSteamLaunch() noexcept;
    bool StopRequested() const noexcept;

private:
    void Publish(SessionPhase phase, SessionResult result = SessionResult::None,
                 const TargetProcess* target = nullptr,
                 const MonitorSample* sample = nullptr,
                 std::wstring detail = {});
    SessionResult Finish(SessionResult result, const TargetProcess* target = nullptr,
                         const MonitorSample* sample = nullptr,
                         std::wstring detail = {});

    IFixSessionPlatform& platform_;
    ISessionObserver& observer_;
    SessionOptions options_;
    std::atomic_bool stopRequested_{false};
    std::atomic_bool steamLaunchRequested_{false};
};

}  // namespace civ6fix
