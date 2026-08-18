#include "FixSession.h"

#include <utility>

namespace civ6fix {
namespace {

SessionResult PreparationFailure(PreparationOutcome outcome) noexcept {
    switch (outcome) {
    case PreparationOutcome::Ready:
        return SessionResult::None;
    case PreparationOutcome::ExistingGame:
        return SessionResult::ExistingGameNoWrite;
    case PreparationOutcome::ProcessScanFailed:
        return SessionResult::ProcessScanFailedNoWrite;
    case PreparationOutcome::InstallationNotFound:
        return SessionResult::InstallationNotFoundNoWrite;
    case PreparationOutcome::UnsupportedBuild:
        return SessionResult::UnsupportedBuildNoWrite;
    case PreparationOutcome::HashFailed:
        return SessionResult::HashFailedNoWrite;
    }
    return SessionResult::ProcessScanFailedNoWrite;
}

}  // namespace

FixSession::FixSession(IFixSessionPlatform& platform, ISessionObserver& observer,
                       SessionOptions options)
    : platform_(platform), observer_(observer), options_(options) {}

void FixSession::RequestStop() noexcept {
    stopRequested_.store(true, std::memory_order_release);
}

void FixSession::RequestSteamLaunch() noexcept {
    steamLaunchRequested_.store(true, std::memory_order_release);
}

bool FixSession::StopRequested() const noexcept {
    return stopRequested_.load(std::memory_order_acquire);
}

void FixSession::Publish(SessionPhase phase, SessionResult result,
                         const TargetProcess* target,
                         const MonitorSample* sample, std::wstring detail) {
    SessionStatus status;
    status.phase = phase;
    status.result = result;
    status.detail = std::move(detail);
    if (target != nullptr) {
        status.profile = target->profile;
        status.pid = target->pid;
    }
    if (sample != nullptr) {
        status.skippedInvalidUnlocks = sample->skippedInvalidUnlocks;
        status.discoveryState = sample->discoveryState;
        status.ssoState = sample->ssoState;
    }
    observer_.OnSessionStatus(status);
}

SessionResult FixSession::Finish(SessionResult result,
                                 const TargetProcess* target,
                                 const MonitorSample* sample,
                                 std::wstring detail) {
    Publish(SessionPhase::Completed, result, target, sample, std::move(detail));
    return result;
}

SessionResult FixSession::Run() {
    Publish(SessionPhase::Preparing);
    const PreparationResult preparation = platform_.Prepare();
    if (preparation.outcome != PreparationOutcome::Ready) {
        return Finish(PreparationFailure(preparation.outcome), nullptr, nullptr,
                      preparation.detail);
    }
    if (StopRequested()) {
        return Finish(SessionResult::CancelledBeforeWrite);
    }

    // This transition is deliberately published before Steam can be invoked. UI guidance
    // is observational and can never gate target detection.
    Publish(SessionPhase::Listening);

    bool initialSteamLaunchPending = options_.launchViaSteam;
    bool steamLaunchAttempted = false;
    std::wstring steamLaunchFailureDetail;
    const auto launchSteamIfRequested = [&]() {
        const bool requestedByUser =
            steamLaunchRequested_.exchange(false, std::memory_order_acq_rel);
        const bool requested = initialSteamLaunchPending || requestedByUser;
        initialSteamLaunchPending = false;
        if (!requested || steamLaunchAttempted) {
            return true;
        }
        steamLaunchAttempted = true;
        std::wstring launchDetail;
        if (!platform_.LaunchViaSteam(launchDetail)) {
            steamLaunchFailureDetail = std::move(launchDetail);
            return false;
        }
        return true;
    };
    if (!launchSteamIfRequested()) {
        return Finish(SessionResult::SteamLaunchFailedNoWrite, nullptr, nullptr,
                      std::move(steamLaunchFailureDetail));
    }

    Publish(SessionPhase::WaitingForGame);
    const std::uint64_t waitStarted = platform_.MonotonicMilliseconds();
    TargetProcess target;
    for (;;) {
        if (StopRequested()) {
            return Finish(SessionResult::CancelledBeforeWrite);
        }
        if (!launchSteamIfRequested()) {
            return Finish(SessionResult::SteamLaunchFailedNoWrite, nullptr,
                          nullptr, std::move(steamLaunchFailureDetail));
        }
        const TargetScanResult scan = platform_.ScanForTarget();
        if (scan.outcome == TargetScanOutcome::Failed) {
            return Finish(SessionResult::ProcessScanFailedNoWrite, nullptr, nullptr,
                          L"target process scan failed");
        }
        if (scan.outcome == TargetScanOutcome::Found) {
            target = scan.target;
            break;
        }
        const std::uint64_t now = platform_.MonotonicMilliseconds();
        if (now - waitStarted >= options_.targetWaitTimeoutMs) {
            return Finish(SessionResult::TargetWaitTimedOutNoWrite);
        }
        platform_.SleepFor(options_.targetPollMs);
    }

    if (target.profile == nullptr) {
        return Finish(SessionResult::UnsupportedBuildNoWrite, &target);
    }
    const BuildUseDecision buildUse = DecideBuildUse(target.profile);
    if (buildUse == BuildUseDecision::RejectUnknownNoWrite) {
        return Finish(SessionResult::UnsupportedBuildNoWrite, &target);
    }

    Publish(SessionPhase::ValidatingRuntime, SessionResult::None, &target);
    const RuntimeValidationResult validation =
        platform_.ValidateRuntime(target, *target.profile);
    if (validation.outcome != RuntimeValidationOutcome::Valid) {
        return Finish(SessionResult::RuntimeMismatchNoWrite, &target, nullptr,
                      validation.detail);
    }

    if (buildUse == BuildUseDecision::ObserveCandidateReadOnly) {
        return Finish(SessionResult::CandidateDetectedReadOnly, &target);
    }
    if (buildUse != BuildUseDecision::InstallVerifiedGuard) {
        return Finish(SessionResult::UnsupportedBuildNoWrite, &target);
    }
    if (StopRequested()) {
        return Finish(SessionResult::CancelledBeforeWrite, &target);
    }

    Publish(SessionPhase::InstallingGuard, SessionResult::None, &target);
    const GuardInstallResult installation =
        platform_.InstallGuard(target, *target.profile);
    if (installation.outcome != GuardInstallOutcome::Installed) {
        SessionResult failure = SessionResult::GuardInstallFailed;
        if (installation.outcome ==
            GuardInstallOutcome::FailedNoHookProtectionUncertain) {
            failure = SessionResult::
                GuardInstallFailedNoHookProtectionUncertain;
        } else if (installation.outcome ==
            GuardInstallOutcome::FailedRestoredAllocationRetained) {
            failure =
                SessionResult::GuardInstallFailedRestoredAllocationRetained;
        } else if (installation.outcome == GuardInstallOutcome::
                                               FailedRestoredProtectionUncertainAllocationRetained) {
            failure = SessionResult::
                GuardInstallFailedRestoredProtectionUncertainAllocationRetained;
        } else if (
            installation.outcome ==
                GuardInstallOutcome::FailedStateUncertainAllocationRetained) {
            failure = SessionResult::
                GuardInstallFailedStateUncertainAllocationRetained;
        }
        return Finish(failure, &target, nullptr, installation.detail);
    }

    Publish(SessionPhase::Monitoring, SessionResult::None, &target);
    const std::uint64_t monitorStarted = platform_.MonotonicMilliseconds();
    MonitorSample current;
    for (;;) {
        if (StopRequested()) {
            const HookCleanupOutcome cleanup =
                platform_.RestoreOwnedHook(target, installation.guard);
            if (cleanup == HookCleanupOutcome::TargetExited) {
                return Finish(SessionResult::TargetExited, &target, &current);
            }
            if (cleanup == HookCleanupOutcome::
                               RestorePointerVerifiedProtectionUncertainRetainAllocation) {
                return Finish(
                    SessionResult::
                        CancelledOwnedHookRestoredProtectionUncertainAllocationRetained,
                    &target, &current);
            }
            const bool restored =
                cleanup == HookCleanupOutcome::RestoreOwnedAndRetainAllocation ||
                cleanup == HookCleanupOutcome::AlreadyOriginalAndRetainAllocation;
            return Finish(
                restored
                    ? SessionResult::CancelledOwnedHookRestoredAllocationRetained
                    : SessionResult::CancelledHookNotOwnedNoWrite,
                &target, &current);
        }

        const TargetRunState runState = platform_.QueryTargetState(target);
        if (runState == TargetRunState::Exited) {
            return Finish(SessionResult::TargetExited, &target, &current);
        }
        if (runState == TargetRunState::WaitFailed) {
            return Finish(SessionResult::TargetWaitFailedHookKept, &target, &current);
        }

        const MonitorSample sample =
            platform_.ReadMonitorSample(target, installation.guard);
        if (sample.readable) {
            const bool changed =
                !current.readable ||
                sample.skippedInvalidUnlocks != current.skippedInvalidUnlocks ||
                sample.discoveryState != current.discoveryState ||
                sample.ssoState != current.ssoState;
            current = sample;
            if (changed) {
                Publish(SessionPhase::Monitoring, SessionResult::None, &target,
                        &current);
            }
        }

        const std::uint64_t now = platform_.MonotonicMilliseconds();
        const MonitorDecision decision = DecideMonitoring({
            current.skippedInvalidUnlocks,
            current.discoveryState,
            current.ssoState,
            now - monitorStarted >= options_.monitorTimeoutMs,
            false,
        });
        if (decision == MonitorDecision::DetachAndKeepHook) {
            return Finish(SessionResult::Fixed, &target, &current);
        }
        if (decision == MonitorDecision::DetachOnlineWithoutIntervention) {
            return Finish(SessionResult::OnlineWithoutIntervention, &target,
                          &current);
        }
        if (decision == MonitorDecision::DetachTimedOutAndKeepHook) {
            return Finish(SessionResult::MonitorTimedOutHookKept, &target, &current);
        }
        platform_.SleepFor(options_.monitorPollMs);
    }
}

}  // namespace civ6fix
