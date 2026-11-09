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
    const auto now = platform_.MonotonicMilliseconds();
    if (phase != activePhase_) {
        diagnostics_.phaseMs[static_cast<std::size_t>(activePhase_)] += now - phaseStartedMs_;
        activePhase_ = phase;
        phaseStartedMs_ = now;
    }
    if (phase != SessionPhase::Completed) diagnostics_.lastActivePhase = static_cast<int>(phase);
    diagnostics_.elapsedMs = now - startedMs_;
    if (monitorStartedMs_) diagnostics_.monitorElapsedMs = now - *monitorStartedMs_;
    status.diagnostics = diagnostics_;
    if (phase == SessionPhase::Completed) {
        status.diagnostics.events = std::move(feedbackEvents_);
        status.diagnostics.eventCount = status.diagnostics.events.size();
        status.diagnostics.omittedEvents = 0;
        status.diagnostics.omittedTail = omittedTail_;
    }
    status.diagnostics.phaseMs[static_cast<std::size_t>(activePhase_)] += now - phaseStartedMs_;
    if (target != nullptr) {
        status.profile = target->profile;
        status.pid = target->pid;
    }
    if (sample != nullptr) {
        status.skippedInvalidUnlocks = sample->counter.quality == ReadQuality::Value
                                          ? sample->skippedInvalidUnlocks : 0;
        status.discoveryState = sample->discovery.quality == ReadQuality::Value ? sample->discoveryState : -1;
        status.ssoState = sample->sso.quality == ReadQuality::Value ? sample->ssoState : -1;
    }
    observer_.OnSessionStatus(status);
}

SessionResult FixSession::Finish(SessionResult result,
                                 const TargetProcess* target,
                                 const MonitorSample* sample,
                                 std::wstring detail) {
    if(target && diagnostics_.guardInstalled && result!=SessionResult::TargetExited)
        diagnostics_.finalOwnership=platform_.InspectHook(*target,installedForReport_);
    Publish(SessionPhase::Completed, result, target, sample, std::move(detail));
    return result;
}

SessionResult FixSession::Run() {
    const auto cancelledBeforeWrite = [this]() {return StopRequested() || preWriteStop_.load(std::memory_order_acquire);};
    startedMs_ = phaseStartedMs_ = platform_.MonotonicMilliseconds();
    diagnostics_ = {};
    feedbackEvents_.clear();
    omittedTail_ = 0;
    diagnostics_.available = true;
    diagnostics_.monitorTimeoutMs = options_.monitorTimeoutMs;
    diagnostics_.monitorPollMs = options_.monitorPollMs;
    diagnostics_.waitTimeoutMs = options_.targetWaitTimeoutMs;
    activePhase_ = SessionPhase::Created;
    monitorStartedMs_.reset();
    Publish(SessionPhase::Preparing);
    const PreparationResult preparation = platform_.Prepare();
    diagnostics_.gameFiles=preparation.gameFiles;
    diagnostics_.scanError=preparation.scanError;
    if (preparation.outcome != PreparationOutcome::Ready) {
        return Finish(PreparationFailure(preparation.outcome), nullptr, nullptr,
                      preparation.detail);
    }
    if (cancelledBeforeWrite()) {
        return Finish(SessionResult::CancelledBeforeWrite);
    }

    // This transition is deliberately published before Steam can be invoked. UI guidance
    // is observational and can never gate target detection.
    Publish(SessionPhase::Listening);
    if (cancelledBeforeWrite()) return Finish(SessionResult::CancelledBeforeWrite);

    bool initialSteamLaunchPending = options_.launchViaSteam;
    bool steamLaunchAttempted = false;
    std::wstring steamLaunchFailureDetail;
    const auto launchSteamIfRequested = [&]() {
        const bool requestedByUser =
            steamLaunchRequested_.exchange(false, std::memory_order_acq_rel);
        const bool requested = initialSteamLaunchPending || requestedByUser;
        initialSteamLaunchPending = false;
        if (!requested || steamLaunchAttempted || cancelledBeforeWrite()) {
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
        if (cancelledBeforeWrite()) {
            return Finish(SessionResult::CancelledBeforeWrite);
        }
        if (!launchSteamIfRequested()) {
            return Finish(SessionResult::SteamLaunchFailedNoWrite, nullptr,
                          nullptr, std::move(steamLaunchFailureDetail));
        }
        const TargetScanResult scan = platform_.ScanForTarget();
        if (scan.outcome == TargetScanOutcome::Failed) {
            diagnostics_.scanError = scan.win32Error;
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
    diagnostics_.validationAttempted = true;
    diagnostics_.validationOutcome = static_cast<int>(validation.outcome);
    diagnostics_.validationError = validation.win32Error;
    diagnostics_.mutexProbe = validation.mutexProbe;
    diagnostics_.mutexCountBeforeGuard = validation.mutexCount;
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
    if (cancelledBeforeWrite()) {
        return Finish(SessionResult::CancelledBeforeWrite, &target);
    }

    Publish(SessionPhase::InstallingGuard, SessionResult::None, &target);
    if (cancelledBeforeWrite()) {
        return Finish(SessionResult::CancelledBeforeWrite, &target);
    }
    const GuardInstallResult installation =
        platform_.InstallGuard(target, *target.profile);
    diagnostics_.installAttempted = true;
    diagnostics_.installOutcome = static_cast<int>(installation.outcome);
    diagnostics_.installError = installation.win32Error;
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

    diagnostics_.guardInstalled = true;
    installedForReport_=installation.guard;
    diagnostics_.processAgeAtGuardMs = platform_.ProcessAgeMilliseconds(target);
    const std::uint64_t monitorStarted = platform_.MonotonicMilliseconds();
    monitorStartedMs_ = monitorStarted;
    Publish(SessionPhase::Monitoring, SessionResult::None, &target);
    std::uint64_t lastPublishedMs = 0;
    MonitorSample current;
    for (;;) {
        diagnostics_.monitorElapsedMs = platform_.MonotonicMilliseconds() - monitorStarted;
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
            if (cleanup == HookCleanupOutcome::UnreadableNoWrite)
                return Finish(SessionResult::CancelledOwnershipUnreadable, &target, &current);
            if (cleanup == HookCleanupOutcome::RestoreAttemptUnconfirmed)
                return Finish(SessionResult::CancelledRestoreUnconfirmed, &target, &current);
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
        current = sample;
        const auto sampledAtMs = platform_.MonotonicMilliseconds() - monitorStarted;
        const bool changed = diagnostics_.Record(sample, sampledAtMs);
        if (changed) {
            // Above all possible polls in the normal 90 s / 10 ms session.
            if (feedbackEvents_.size() < 16384) feedbackEvents_.push_back({sampledAtMs, sample});
            else ++omittedTail_;
        }
        const bool ready = CompleteSample(sample) && sample.discoveryState == 4 && sample.ssoState == 4;
        // Bound UI/log volume, but never suppress the first or ready observation.
        if (diagnostics_.samples == 1 || ready ||
            (changed && sampledAtMs - lastPublishedMs >= 250) ||
            sampledAtMs - lastPublishedMs >= 1000) {
            Publish(SessionPhase::Monitoring, SessionResult::None, &target, &current);
            lastPublishedMs = sampledAtMs;
        }

        const std::uint64_t now = platform_.MonotonicMilliseconds();
        diagnostics_.monitorElapsedMs = now - monitorStarted;
        // An observer or the UI may cancel during the sample read/publication.
        // Return to the owned-hook cleanup path before selecting a terminal result.
        if (StopRequested()) {
            continue;
        }
        const MonitorDecision decision = DecideMonitoring({
            current.skippedInvalidUnlocks,
            CompleteSample(current) ? current.discoveryState : -1,
            CompleteSample(current) ? current.ssoState : -1,
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
