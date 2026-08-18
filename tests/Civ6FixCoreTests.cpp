#include "Civ6FixCore.h"
#include "FixSession.h"
#include "SessionStatusMailbox.h"
#include "SessionPresentation.h"
#include "SteamDiscovery.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>

namespace {

class TestRunner {
public:
    void Expect(bool condition, const std::string& name) {
        if (condition) {
            std::cout << "PASS: " << name << '\n';
        } else {
            std::cerr << "FAIL: " << name << '\n';
            ++failures_;
        }
    }

    int Finish() const {
        if (failures_ == 0) {
            std::cout << "ALL TESTS PASSED\n";
            return 0;
        }
        std::cerr << failures_ << " TEST(S) FAILED\n";
        return 1;
    }

private:
    int failures_ = 0;
};

std::uint64_t ReadImmediate64(const std::vector<std::uint8_t>& code,
                              std::size_t offset) {
    std::uint64_t value = 0;
    if (offset + sizeof(value) <= code.size()) {
        std::memcpy(&value, code.data() + offset, sizeof(value));
    }
    return value;
}

class ListenerOrderObserver final : public civ6fix::ISessionObserver {
public:
    explicit ListenerOrderObserver(std::vector<std::string>& order) : order_(order) {}

    void OnSessionStatus(const civ6fix::SessionStatus& status) override {
        if (status.phase == civ6fix::SessionPhase::Listening) {
            order_.push_back("listening");
        }
    }

private:
    std::vector<std::string>& order_;
};

class ListenerOrderPlatform final : public civ6fix::IFixSessionPlatform {
public:
    explicit ListenerOrderPlatform(std::vector<std::string>& order) : order_(order) {}

    civ6fix::PreparationResult Prepare() override {
        return {civ6fix::PreparationOutcome::Ready, L""};
    }

    bool LaunchViaSteam(std::wstring&) override {
        order_.push_back("launch");
        return true;
    }

    civ6fix::TargetScanResult ScanForTarget() override {
        return {civ6fix::TargetScanOutcome::Found,
                {42, targetProfile, L"scripted-target"}, 0};
    }

    civ6fix::RuntimeValidationResult ValidateRuntime(
        const civ6fix::TargetProcess&, const civ6fix::BuildProfile&) override {
        ++validationCalls;
        return {validationOutcome, 0, L"scripted validation"};
    }

    civ6fix::GuardInstallResult InstallGuard(
        const civ6fix::TargetProcess&, const civ6fix::BuildProfile&) override {
        ++installCalls;
        return {installOutcome, {0x1000, 0x2000, 0x3000, 0x3000, 0x4000},
                0, L"scripted install"};
    }

    civ6fix::TargetRunState QueryTargetState(
        const civ6fix::TargetProcess&) override {
        if (monitorReadsBeforeExit >= 0) {
            return monitorReadCalls >= monitorReadsBeforeExit
                       ? civ6fix::TargetRunState::Exited
                       : civ6fix::TargetRunState::Running;
        }
        return targetRunState;
    }

    civ6fix::MonitorSample ReadMonitorSample(
        const civ6fix::TargetProcess&, const civ6fix::InstalledGuard&) override {
        ++monitorReadCalls;
        return monitorSample;
    }

    civ6fix::HookCleanupOutcome RestoreOwnedHook(
        const civ6fix::TargetProcess&, const civ6fix::InstalledGuard&) override {
        ++restoreCalls;
        return cleanupOutcome;
    }

    std::uint64_t MonotonicMilliseconds() override { return now_++; }
    void SleepFor(std::uint32_t) override {}

    const civ6fix::BuildProfile* targetProfile = &civ6fix::kDx12Profile;
    civ6fix::RuntimeValidationOutcome validationOutcome =
        civ6fix::RuntimeValidationOutcome::Valid;
    civ6fix::GuardInstallOutcome installOutcome =
        civ6fix::GuardInstallOutcome::FailedNoHook;
    civ6fix::TargetRunState targetRunState = civ6fix::TargetRunState::Exited;
    civ6fix::HookCleanupOutcome cleanupOutcome =
        civ6fix::HookCleanupOutcome::TargetExited;
    int validationCalls = 0;
    int installCalls = 0;
    int restoreCalls = 0;
    int monitorReadsBeforeExit = -1;
    int monitorReadCalls = 0;
    civ6fix::MonitorSample monitorSample;

private:
    std::vector<std::string>& order_;
    std::uint64_t now_ = 0;
};

class SilentObserver final : public civ6fix::ISessionObserver {
public:
    void OnSessionStatus(const civ6fix::SessionStatus&) override {}
};

class MonitoringCountObserver final : public civ6fix::ISessionObserver {
public:
    void OnSessionStatus(const civ6fix::SessionStatus& status) override {
        if (status.phase == civ6fix::SessionPhase::Monitoring) {
            ++monitoringUpdates;
        }
    }

    int monitoringUpdates = 0;
};

class StopAtMonitoringObserver final : public civ6fix::ISessionObserver {
public:
    void OnSessionStatus(const civ6fix::SessionStatus& status) override {
        if (!stopped_ && status.phase == civ6fix::SessionPhase::Monitoring &&
            session != nullptr) {
            stopped_ = true;
            session->RequestStop();
        }
    }

    civ6fix::FixSession* session = nullptr;

private:
    bool stopped_ = false;
};

class LaunchAtListeningObserver final : public civ6fix::ISessionObserver {
public:
    explicit LaunchAtListeningObserver(std::vector<std::string>& order)
        : order_(order) {}

    void OnSessionStatus(const civ6fix::SessionStatus& status) override {
        if (!requested_ && status.phase == civ6fix::SessionPhase::Listening &&
            session != nullptr) {
            order_.push_back("listening");
            requested_ = true;
            session->RequestSteamLaunch();
        }
    }

    civ6fix::FixSession* session = nullptr;

private:
    std::vector<std::string>& order_;
    bool requested_ = false;
};

}  // namespace

int main() {
    TestRunner tests;

    const auto steamLibraries = civ6fix::ParseSteamLibraryFolders(
        R"VDF("libraryfolders"
{
    "0" { "path" "C:\\Program Files (x86)\\Steam" }
    "1" { "path" "T:\\SteamLibrary" }
})VDF");
    tests.Expect(steamLibraries.size() == 2 &&
                     steamLibraries[1] == L"T:\\SteamLibrary",
                 "modern Steam libraryfolders VDF yields portable library paths");

    tests.Expect(
        civ6fix::ParseSteamLibraryFolders(
            std::string(civ6fix::kMaxSteamMetadataBytes + 1, 'x'))
            .empty(),
        "oversized Steam metadata is rejected before token expansion");

    std::string excessiveLibraries = "\"libraryfolders\" {";
    for (std::size_t index = 0;
         index < civ6fix::kMaxSteamLibraryCount + 1; ++index) {
        excessiveLibraries += "\"" + std::to_string(index) +
                              "\" { \"path\" \"C:\\\\Steam" +
                              std::to_string(index) + "\" }";
    }
    excessiveLibraries += "}";
    tests.Expect(
        civ6fix::ParseSteamLibraryFolders(excessiveLibraries).size() ==
            civ6fix::kMaxSteamLibraryCount,
        "Steam library discovery has a fixed path-count budget");
    tests.Expect(
        civ6fix::ParseSteamLibraryFolders(
            R"VDF("libraryfolders" { "0" { "path" "\\\\server\\Steam" } })VDF")
            .empty(),
        "direct UNC Steam libraries are rejected to avoid unbounded network probes");
    tests.Expect(
        civ6fix::ParseSteamAppInstallDir(
            R"VDF("AppState" { "appid" "289070" "installdir" "Sid Meier's Civilization VI" })VDF") ==
            std::optional<std::wstring>(L"Sid Meier's Civilization VI"),
        "Steam manifest binds installdir to the Civilization VI app id");
    tests.Expect(
        !civ6fix::ParseSteamAppInstallDir(
             R"VDF("AppState" { "appid" "123" "installdir" "Sid Meier's Civilization VI" })VDF")
             .has_value(),
        "a mismatched Steam app manifest cannot select a game directory");

    civ6fix::SessionStatusMailbox mailbox;
    tests.Expect(!mailbox.TryPop().has_value(),
                 "unsolicited UI wake-up is harmless when the mailbox is empty");
    civ6fix::SessionStatus firstStatus;
    firstStatus.detail = L"first";
    civ6fix::SessionStatus secondStatus;
    secondStatus.detail = L"second";
    mailbox.Push(firstStatus);
    mailbox.Push(secondStatus);
    const auto poppedFirst = mailbox.TryPop();
    const auto poppedSecond = mailbox.TryPop();
    tests.Expect(poppedFirst.has_value() && poppedSecond.has_value() &&
                     poppedFirst->detail == L"first" &&
                     poppedSecond->detail == L"second" &&
                     !mailbox.TryPop().has_value(),
                 "worker statuses remain owned in-process and drain in FIFO order");

    const auto candidatePresentation = civ6fix::PresentSessionResult(
        civ6fix::SessionResult::CandidateDetectedReadOnly);
    tests.Expect(candidatePresentation.detail.find(L"未写入") !=
                     std::wstring::npos,
                 "a read-only candidate result explicitly says the process was not modified");

    civ6fix::SessionStatus fixedStatus;
    fixedStatus.phase = civ6fix::SessionPhase::Completed;
    fixedStatus.result = civ6fix::SessionResult::Fixed;
    const auto fixedControls = civ6fix::PresentSessionControls(fixedStatus);
    const auto fixedPresentation =
        civ6fix::PresentSessionStatus(fixedStatus);
    tests.Expect(
        fixedControls.primaryAction ==
                civ6fix::SessionPrimaryAction::CloseWindow &&
            fixedControls.primaryLabel == L"关闭窗口" &&
            fixedControls.primaryEnabled,
        "a completed repair offers an enabled Close Window action");
    tests.Expect(
        fixedPresentation.detail.find(L"现在可以关闭窗口") !=
                std::wstring::npos &&
            fixedPresentation.detail.find(L"游戏退出时") !=
                std::wstring::npos,
        "repair success explains that the window may close before the game exits");

    fixedStatus.pid = 42;
    fixedStatus.profile = &civ6fix::kVerifiedDx11Profile;
    fixedStatus.skippedInvalidUnlocks = 1;
    fixedStatus.discoveryState = 4;
    fixedStatus.ssoState = 4;
    fixedStatus.detail =
        L"R:\\PrivateProfile\\ExampleUser\\private.jsonl at 0x12345678";
    const std::wstring shareableDiagnostic =
        civ6fix::BuildShareableDiagnostic(L"9.8.7-test", fixedStatus, false);
    tests.Expect(
        shareableDiagnostic.find(L"9.8.7-test") != std::wstring::npos &&
            shareableDiagnostic.find(L"DirectX 11") != std::wstring::npos &&
            shareableDiagnostic.find(L"Verified") != std::wstring::npos &&
            shareableDiagnostic.find(L"Discovery：4") != std::wstring::npos &&
            shareableDiagnostic.find(L"SSO：4") != std::wstring::npos &&
            shareableDiagnostic.find(L"PID：42") == std::wstring::npos &&
            shareableDiagnostic.find(L"ExampleUser") == std::wstring::npos &&
            shareableDiagnostic.find(L"private.jsonl") == std::wstring::npos &&
            shareableDiagnostic.find(L"0x12345678") == std::wstring::npos &&
            shareableDiagnostic.find(L"日志路径、PID 和内存地址") !=
                std::wstring::npos,
        "the copied diagnostic is shareable without local paths, PID, addresses, or raw platform detail");

    const auto naturalOnlinePresentation = civ6fix::PresentSessionResult(
        civ6fix::SessionResult::OnlineWithoutIntervention);
    tests.Expect(
        naturalOnlinePresentation.detail.find(L"现在可以关闭窗口") !=
            std::wstring::npos,
        "natural online success offers the same unambiguous close guidance");

    const auto exitedPresentation = civ6fix::PresentSessionResult(
        civ6fix::SessionResult::TargetExited);
    tests.Expect(
        exitedPresentation.detail.find(L"关闭窗口") != std::wstring::npos,
        "a finished game explicitly tells the user how to close the helper");
    tests.Expect(
        civ6fix::ExitCodeForSessionResult(civ6fix::SessionResult::TargetExited) !=
            0,
        "headless mode does not report a pre-result target exit as success");

    civ6fix::SessionStatus monitoringStatus;
    monitoringStatus.phase = civ6fix::SessionPhase::Monitoring;
    const auto monitoringControls =
        civ6fix::PresentSessionControls(monitoringStatus);
    tests.Expect(
        monitoringControls.primaryAction ==
                civ6fix::SessionPrimaryAction::RequestStop &&
            monitoringControls.primaryLabel == L"停止并撤销保护" &&
            monitoringControls.primaryEnabled,
        "an installed guard offers an explicit stop-and-restore action");

    civ6fix::SessionStatus waitingStatus;
    waitingStatus.phase = civ6fix::SessionPhase::WaitingForGame;
    const auto waitingControls = civ6fix::PresentSessionControls(waitingStatus);
    const auto waitingLaunchControl =
        civ6fix::PresentSessionLaunchControl(waitingStatus, false);
    const auto waitingPresentation =
        civ6fix::PresentSessionStatus(waitingStatus);
    tests.Expect(
        waitingControls.primaryAction ==
                civ6fix::SessionPrimaryAction::RequestStop &&
            waitingControls.primaryLabel == L"取消本次" &&
            waitingControls.primaryEnabled,
        "a pre-write session offers a clear cancellation action");
    tests.Expect(
        waitingLaunchControl.visible && waitingLaunchControl.enabled &&
            waitingLaunchControl.action ==
                civ6fix::SessionLaunchAction::RequestSteamDefault &&
            waitingLaunchControl.label.find(L"默认项") != std::wstring::npos &&
            waitingPresentation.detail.find(L"手动") != std::wstring::npos &&
            waitingPresentation.detail.find(L"不保证") != std::wstring::npos,
        "the waiting GUI recommends manual renderer choice and labels the explicit Steam default action truthfully");
    const auto requestedLaunchControl =
        civ6fix::PresentSessionLaunchControl(waitingStatus, true);
    tests.Expect(
        requestedLaunchControl.visible && !requestedLaunchControl.enabled &&
            requestedLaunchControl.action ==
                civ6fix::SessionLaunchAction::None &&
            requestedLaunchControl.label.find(L"已请求") != std::wstring::npos,
        "the Steam default action disables immediately after one request");

    civ6fix::SessionStatus retryStatus;
    retryStatus.phase = civ6fix::SessionPhase::Completed;
    retryStatus.result = civ6fix::SessionResult::ExistingGameNoWrite;
    const auto retryLaunchControl =
        civ6fix::PresentSessionLaunchControl(retryStatus, false);
    tests.Expect(
        retryLaunchControl.visible && retryLaunchControl.enabled &&
            retryLaunchControl.action ==
                civ6fix::SessionLaunchAction::RetrySession &&
            retryLaunchControl.label == L"重新检测",
        "the shared secondary button becomes Retry only for retryable terminal results");

    std::vector<std::string> startupOrder;
    ListenerOrderObserver listenerObserver(startupOrder);
    ListenerOrderPlatform listenerPlatform(startupOrder);
    civ6fix::BuildProfile listenerCandidate = civ6fix::kDx12Profile;
    listenerCandidate.supportState = civ6fix::SupportState::ReadOnlyCandidate;
    listenerPlatform.targetProfile = &listenerCandidate;
    civ6fix::SessionOptions listenerOptions;
    listenerOptions.launchViaSteam = true;
    civ6fix::FixSession listenerSession(listenerPlatform, listenerObserver,
                                        listenerOptions);
    const auto listenerResult = listenerSession.Run();
    tests.Expect(listenerResult ==
                         civ6fix::SessionResult::CandidateDetectedReadOnly &&
                     startupOrder ==
                         std::vector<std::string>{"listening", "launch"},
                 "listener becomes active before any Steam launch or UI acknowledgement");

    std::vector<std::string> defaultStartupOrder;
    ListenerOrderObserver defaultListenerObserver(defaultStartupOrder);
    ListenerOrderPlatform defaultListenerPlatform(defaultStartupOrder);
    defaultListenerPlatform.targetProfile = &listenerCandidate;
    civ6fix::FixSession defaultListenerSession(
        defaultListenerPlatform, defaultListenerObserver);
    const auto defaultListenerResult = defaultListenerSession.Run();
    tests.Expect(
        defaultListenerResult ==
                civ6fix::SessionResult::CandidateDetectedReadOnly &&
            defaultStartupOrder == std::vector<std::string>{"listening"},
        "the public default listens without requesting a Steam launch");

    std::vector<std::string> explicitLaunchOrder;
    LaunchAtListeningObserver explicitLaunchObserver(explicitLaunchOrder);
    ListenerOrderPlatform explicitLaunchPlatform(explicitLaunchOrder);
    explicitLaunchPlatform.targetProfile = &listenerCandidate;
    civ6fix::FixSession explicitLaunchSession(
        explicitLaunchPlatform, explicitLaunchObserver);
    explicitLaunchObserver.session = &explicitLaunchSession;
    const auto explicitLaunchResult = explicitLaunchSession.Run();
    tests.Expect(
        explicitLaunchResult ==
                civ6fix::SessionResult::CandidateDetectedReadOnly &&
            explicitLaunchOrder ==
                std::vector<std::string>{"listening", "launch"},
        "an explicit Steam request is honored only after listening is active");

    std::vector<std::string> unknownOrder;
    ListenerOrderPlatform unknownPlatform(unknownOrder);
    unknownPlatform.targetProfile = nullptr;
    SilentObserver silentObserver;
    civ6fix::FixSession unknownSession(unknownPlatform, silentObserver,
                                      listenerOptions);
    tests.Expect(
        unknownSession.Run() == civ6fix::SessionResult::UnsupportedBuildNoWrite &&
            unknownPlatform.validationCalls == 0 &&
            unknownPlatform.installCalls == 0,
        "unknown target stops before runtime validation or hook installation");

    std::vector<std::string> unsupportedOrder;
    ListenerOrderPlatform unsupportedPlatform(unsupportedOrder);
    civ6fix::BuildProfile unsupportedProfile = civ6fix::kVerifiedDx11Profile;
    unsupportedProfile.supportState = civ6fix::SupportState::Unsupported;
    unsupportedPlatform.targetProfile = &unsupportedProfile;
    civ6fix::FixSession unsupportedSession(
        unsupportedPlatform, silentObserver, listenerOptions);
    tests.Expect(
        unsupportedSession.Run() ==
                civ6fix::SessionResult::UnsupportedBuildNoWrite &&
            unsupportedPlatform.installCalls == 0,
        "an explicit Unsupported profile cannot reach InstallGuard");

    std::vector<std::string> mismatchOrder;
    ListenerOrderPlatform mismatchPlatform(mismatchOrder);
    mismatchPlatform.targetProfile = &civ6fix::kVerifiedDx11Profile;
    mismatchPlatform.validationOutcome =
        civ6fix::RuntimeValidationOutcome::PeIdentityMismatch;
    civ6fix::FixSession mismatchSession(mismatchPlatform, silentObserver,
                                       listenerOptions);
    tests.Expect(
        mismatchSession.Run() == civ6fix::SessionResult::RuntimeMismatchNoWrite &&
            mismatchPlatform.installCalls == 0,
        "runtime mismatch fails closed before InstallGuard is called");

    std::vector<std::string> uncertainInstallOrder;
    ListenerOrderPlatform uncertainInstallPlatform(uncertainInstallOrder);
    uncertainInstallPlatform.targetProfile = &civ6fix::kVerifiedDx11Profile;
    uncertainInstallPlatform.installOutcome =
        civ6fix::GuardInstallOutcome::FailedStateUncertainAllocationRetained;
    civ6fix::FixSession uncertainInstallSession(
        uncertainInstallPlatform, silentObserver, listenerOptions);
    tests.Expect(
        uncertainInstallSession.Run() ==
            civ6fix::SessionResult::GuardInstallFailedStateUncertainAllocationRetained,
        "an ambiguous IAT publication is reported as uncertain instead of restored");

    std::vector<std::string> protectionInstallOrder;
    ListenerOrderPlatform protectionInstallPlatform(protectionInstallOrder);
    protectionInstallPlatform.targetProfile = &civ6fix::kVerifiedDx11Profile;
    protectionInstallPlatform.installOutcome =
        civ6fix::GuardInstallOutcome::FailedNoHookProtectionUncertain;
    civ6fix::FixSession protectionInstallSession(
        protectionInstallPlatform, silentObserver, listenerOptions);
    tests.Expect(
        protectionInstallSession.Run() == civ6fix::SessionResult::
            GuardInstallFailedNoHookProtectionUncertain,
        "a failed IAT page-protection restore has a truthful session result");

    std::vector<std::string> restoredProtectionInstallOrder;
    ListenerOrderPlatform restoredProtectionInstallPlatform(
        restoredProtectionInstallOrder);
    restoredProtectionInstallPlatform.targetProfile =
        &civ6fix::kVerifiedDx11Profile;
    restoredProtectionInstallPlatform.installOutcome = civ6fix::
        GuardInstallOutcome::FailedRestoredProtectionUncertainAllocationRetained;
    civ6fix::FixSession restoredProtectionInstallSession(
        restoredProtectionInstallPlatform, silentObserver, listenerOptions);
    tests.Expect(
        restoredProtectionInstallSession.Run() == civ6fix::SessionResult::
            GuardInstallFailedRestoredProtectionUncertainAllocationRetained,
        "a restored IAT with uncertain page protection has a truthful session result");

    std::vector<std::string> waitFailureOrder;
    ListenerOrderPlatform waitFailurePlatform(waitFailureOrder);
    waitFailurePlatform.targetProfile = &civ6fix::kVerifiedDx11Profile;
    waitFailurePlatform.installOutcome = civ6fix::GuardInstallOutcome::Installed;
    waitFailurePlatform.targetRunState = civ6fix::TargetRunState::WaitFailed;
    civ6fix::FixSession waitFailureSession(waitFailurePlatform, silentObserver,
                                          listenerOptions);
    tests.Expect(
        waitFailureSession.Run() ==
            civ6fix::SessionResult::TargetWaitFailedHookKept,
        "WAIT_FAILED is distinct from target exit and keeps the published guard");

    std::vector<std::string> unchangedMonitorOrder;
    ListenerOrderPlatform unchangedMonitorPlatform(unchangedMonitorOrder);
    unchangedMonitorPlatform.targetProfile = &civ6fix::kVerifiedDx11Profile;
    unchangedMonitorPlatform.installOutcome =
        civ6fix::GuardInstallOutcome::Installed;
    unchangedMonitorPlatform.monitorReadsBeforeExit = 3;
    unchangedMonitorPlatform.monitorSample = {true, 0, 3, 3};
    MonitoringCountObserver monitoringCountObserver;
    civ6fix::FixSession unchangedMonitorSession(
        unchangedMonitorPlatform, monitoringCountObserver, listenerOptions);
    tests.Expect(
        unchangedMonitorSession.Run() == civ6fix::SessionResult::TargetExited &&
            monitoringCountObserver.monitoringUpdates == 2,
        "unchanged monitor samples do not flood observers or structured logs");

    std::vector<std::string> foreignCleanupOrder;
    ListenerOrderPlatform foreignCleanupPlatform(foreignCleanupOrder);
    foreignCleanupPlatform.targetProfile = &civ6fix::kVerifiedDx11Profile;
    foreignCleanupPlatform.installOutcome =
        civ6fix::GuardInstallOutcome::Installed;
    foreignCleanupPlatform.targetRunState = civ6fix::TargetRunState::Running;
    foreignCleanupPlatform.cleanupOutcome =
        civ6fix::HookCleanupOutcome::ForeignOwnerNoWrite;
    StopAtMonitoringObserver stopObserver;
    civ6fix::FixSession foreignCleanupSession(foreignCleanupPlatform,
                                              stopObserver, listenerOptions);
    stopObserver.session = &foreignCleanupSession;
    tests.Expect(
        foreignCleanupSession.Run() ==
                civ6fix::SessionResult::CancelledHookNotOwnedNoWrite &&
            foreignCleanupPlatform.restoreCalls == 1,
        "cancellation never overwrites a foreign IAT owner");

    std::vector<std::string> protectionCleanupOrder;
    ListenerOrderPlatform protectionCleanupPlatform(protectionCleanupOrder);
    protectionCleanupPlatform.targetProfile = &civ6fix::kVerifiedDx11Profile;
    protectionCleanupPlatform.installOutcome =
        civ6fix::GuardInstallOutcome::Installed;
    protectionCleanupPlatform.targetRunState =
        civ6fix::TargetRunState::Running;
    protectionCleanupPlatform.cleanupOutcome = civ6fix::HookCleanupOutcome::
        RestorePointerVerifiedProtectionUncertainRetainAllocation;
    StopAtMonitoringObserver protectionStopObserver;
    civ6fix::FixSession protectionCleanupSession(
        protectionCleanupPlatform, protectionStopObserver, listenerOptions);
    protectionStopObserver.session = &protectionCleanupSession;
    tests.Expect(
        protectionCleanupSession.Run() == civ6fix::SessionResult::
            CancelledOwnedHookRestoredProtectionUncertainAllocationRetained,
        "cancellation distinguishes a restored IAT from uncertain page protection");

    tests.Expect(
        civ6fix::DecideInstanceStartup(civ6fix::InstanceLockState::AlreadyOwned) ==
            civ6fix::InstanceStartupDecision::ActivateExistingAndExit,
        "second launcher activates the existing instance and exits");
    tests.Expect(civ6fix::ExitCodeForExistingInstance(true) != 0 &&
                     civ6fix::ExitCodeForExistingInstance(false) == 0,
                 "a headless duplicate instance does not report false success");

    const auto* dx11Profile = civ6fix::FindBuildProfile(
        L"CivilizationVI.exe",
        L"E7450823CC8E00468CFF7B9D7B97C63140EAE38AE1D774BA4EFA437556C42D63");
    tests.Expect(dx11Profile != nullptr &&
                     dx11Profile->supportState == civ6fix::SupportState::Verified,
                 "exact validated DX11 build selects a writable verified profile");
    tests.Expect(
        civ6fix::kVerifiedDx11Profile.fileSize == 21160312ULL &&
            civ6fix::kVerifiedDx11Profile.peTimestamp == 0x667C6FC6U &&
            civ6fix::kVerifiedDx11Profile.preferredImageBase == 0x140000000ULL &&
            civ6fix::kVerifiedDx11Profile.imageSize == 0x03704000U &&
            civ6fix::kVerifiedDx11Profile.dllCharacteristics == 0x8120U &&
            civ6fix::kVerifiedDx11Profile.mtxUnlockIatRva == 0x00F96E70U &&
            civ6fix::kVerifiedDx11Profile.opensslUnlockReturnRva == 0x00BCEEC5U &&
            civ6fix::kVerifiedDx11Profile.opensslLockVectorRva == 0x03551668U &&
            civ6fix::kVerifiedDx11Profile.discoveryGlobalRva == 0x03551700U &&
            civ6fix::kVerifiedDx11Profile.ssoGlobalRva == 0x03551598U &&
            civ6fix::kVerifiedDx11Profile.mutexOwnerOffset == 0x48U &&
            civ6fix::kVerifiedDx11Profile.mutexCountOffset == 0x4CU &&
            civ6fix::kVerifiedDx11Profile.opensslLockIndex == 18U,
        "DX11 Verified Profile keeps its complete audited contract");

    const auto* dx12Profile = civ6fix::FindBuildProfile(
        L"CivilizationVI_DX12.exe",
        L"C2C3D40B86260A541D8A4D38CB70D50D3406AE1DE374AFC382D1E42BC1342F1E");
    tests.Expect(
        dx12Profile != nullptr &&
            dx12Profile->supportState == civ6fix::SupportState::Verified &&
            civ6fix::DecideBuildUse(dx12Profile) ==
                civ6fix::BuildUseDecision::InstallVerifiedGuard,
        "exact validated DX12 build selects a writable verified profile");
    tests.Expect(
        civ6fix::kDx12Profile.fileSize == 21391464ULL &&
            civ6fix::kDx12Profile.peTimestamp == 0x667C7363U &&
            civ6fix::kDx12Profile.preferredImageBase == 0x140000000ULL &&
            civ6fix::kDx12Profile.imageSize == 0x0FE55000U &&
            civ6fix::kDx12Profile.dllCharacteristics == 0x8120U &&
            civ6fix::kDx12Profile.mtxUnlockIatRva == 0x00FA5E78U &&
            civ6fix::kDx12Profile.opensslUnlockReturnRva == 0x00BDDA15U &&
            civ6fix::kDx12Profile.opensslLockVectorRva == 0x0FCA0FE8U &&
            civ6fix::kDx12Profile.discoveryGlobalRva == 0x0FCA1080U &&
            civ6fix::kDx12Profile.ssoGlobalRva == 0x0FCA0F18U &&
            civ6fix::kDx12Profile.mutexOwnerOffset == 0x48U &&
            civ6fix::kDx12Profile.mutexCountOffset == 0x4CU &&
            civ6fix::kDx12Profile.opensslLockIndex == 18U,
        "DX12 Verified Profile keeps its complete audited contract");
    civ6fix::BuildProfile experimentalDx12 = civ6fix::kDx12Profile;
    experimentalDx12.supportState = civ6fix::SupportState::Experimental;
    tests.Expect(
        civ6fix::DecideBuildUse(&experimentalDx12) ==
            civ6fix::BuildUseDecision::ObserveCandidateReadOnly,
        "an experimental profile remains read-only in the public build");
    tests.Expect(
        civ6fix::DecideBuildUse(civ6fix::FindBuildProfile(
            L"CivilizationVI.exe",
            L"0000000000000000000000000000000000000000000000000000000000000000")) ==
            civ6fix::BuildUseDecision::RejectUnknownNoWrite,
        "unknown game hash is rejected before any process write");

    tests.Expect(
        civ6fix::DecideRuntimeFileIdentity({true, true, false, true}) ==
            civ6fix::RuntimeFileIdentityDecision::RejectDigestMismatch,
        "runtime digest mismatch denies writes even when metadata still matches");
    tests.Expect(
        civ6fix::DecideRuntimeFileIdentity({true, true, true, true}) ==
            civ6fix::RuntimeFileIdentityDecision::AcceptExactIdentity,
        "runtime writes require fingerprint, size, digest, and main-module binding");
    tests.Expect(
        civ6fix::DecideRuntimeProcessIdentity({true, false, true}) ==
            civ6fix::RuntimeProcessIdentityDecision::RejectCreationTimeMismatch,
        "a recycled PID cannot inherit a scanned target identity");
    tests.Expect(
        civ6fix::DecideRuntimeProcessIdentity({true, true, true}) ==
            civ6fix::RuntimeProcessIdentityDecision::AcceptSameProcess,
        "runtime validation accepts the scanned PID only with matching creation time and path");
    tests.Expect(
        civ6fix::DecideRuntimeLibraryIdentity({true, false, true, true}) ==
            civ6fix::RuntimeLibraryIdentityDecision::RejectSystemPathMismatch,
        "an app-local MSVCP140.dll is rejected even when its export and image checks match");
    tests.Expect(
        civ6fix::DecideRuntimeLibraryIdentity({true, true, true, true}) ==
            civ6fix::RuntimeLibraryIdentityDecision::AcceptExactIdentity,
        "the runtime library contract accepts only the exact System32 export in executable image memory");
    tests.Expect(
        civ6fix::DecideRuntimeLibraryIdentity({true, true, false, true}) ==
            civ6fix::RuntimeLibraryIdentityDecision::RejectExportMismatch,
        "a System32 module with the wrong _Mtx_unlock export target is rejected");
    tests.Expect(
        civ6fix::DecideRuntimeIatTarget(false, false) ==
                civ6fix::RuntimeIatDecision::KeepWaiting &&
            civ6fix::DecideRuntimeIatTarget(true, false) ==
                civ6fix::RuntimeIatDecision::AcceptExactTarget &&
            civ6fix::DecideRuntimeIatTarget(false, true) ==
                civ6fix::RuntimeIatDecision::RejectMismatch,
        "a transient loader IAT owner is retried but a persistent mismatch fails closed");
    tests.Expect(
        !civ6fix::IsRvaSpanWithinImage(0x1000, 0x0FFC,
                                       sizeof(std::uintptr_t)),
        "an IAT pointer that crosses the remote image boundary is rejected");
    tests.Expect(
        civ6fix::IsPointerVectorIndexAvailable(0x1000, 0x1100, 18) &&
            !civ6fix::IsPointerVectorIndexAvailable(0x1000, 0x1101, 18) &&
            !civ6fix::IsPointerVectorIndexAvailable(0x1000, 0x1080, 18),
        "the optional mutex probe rejects misaligned or undersized vectors");
    tests.Expect(
        civ6fix::IsAddressAdditionSafe(
            (std::numeric_limits<std::uintptr_t>::max)() - 3U, 3U) &&
            !civ6fix::IsAddressAdditionSafe(
                (std::numeric_limits<std::uintptr_t>::max)() - 3U, 4U),
        "remote pointer arithmetic cannot wrap into a different address range");

    const auto ownedCleanup = civ6fix::PlanHookCleanup(
        true, civ6fix::IatOwnership::OwnedBySession);
    tests.Expect(ownedCleanup.restoreOriginalIat &&
                     !ownedCleanup.freeRemoteAllocation,
                 "cancellation restores only the owned IAT and never hot-frees code");

    tests.Expect(
        civ6fix::EvaluatePointerPatch({true, true, false, true, true}) ==
            civ6fix::PointerPatchOutcome::ProtectionRestoreFailed,
        "page protection restoration failure makes an IAT patch fail");
    tests.Expect(
        civ6fix::EvaluatePointerPatch({true, false, true, true, false, false}) ==
            civ6fix::PointerPatchOutcome::OwnershipChanged,
        "IAT ownership change aborts the write instead of overwriting a foreign hook");
    tests.Expect(
        !civ6fix::CanFreeGuardAfterFailedPublish(
            {true, false, true, false, false, true}, 0, 0x1234),
        "ambiguous partial IAT write never permits freeing guard memory");
    tests.Expect(
        civ6fix::CanFreeGuardAfterFailedPublish(
            {false, false, false, false, false, true}, 0, 0x1234),
        "guard memory may be freed when page protection failed before any publish attempt");
    tests.Expect(
        civ6fix::DecideFailedPublishDisposition(
            {true, false, true, true, false, true}, 0x12AA, 0x1234,
            false, {}, 0) ==
            civ6fix::FailedPublishDisposition::StateUncertainRetainAllocation,
        "a partial pointer value is preserved as uncertain and retains guard memory");
    tests.Expect(
        civ6fix::DecideFailedPublishDisposition(
            {true, false, false, true, false, false}, 0x7777, 0x1234,
            false, {}, 0) ==
            civ6fix::FailedPublishDisposition::
                NoHookProtectionUncertainSafeToFree,
        "a failed page-protection restore is not hidden when no hook was published");
    tests.Expect(
        civ6fix::DecideFailedPublishDisposition(
            {true, true, true, true, true, true}, 0x5678, 0x1234,
            true, {true, true, true, true, true, true}, 0x1234) ==
            civ6fix::FailedPublishDisposition::OriginalRestoredRetainAllocation,
        "a failed publish is called restored only after final readback sees the original target");
    tests.Expect(
        civ6fix::DecideFailedPublishDisposition(
            {true, true, false, true, true, true}, 0x5678, 0x1234,
            true, {true, true, false, true, true, true}, 0x1234) ==
            civ6fix::FailedPublishDisposition::
                OriginalRestoredProtectionUncertainRetainAllocation,
        "restoring the original pointer does not hide uncertain page protection");

    tests.Expect(
        civ6fix::DecidePreflight(civ6fix::GamePresence::Running,
                                 civ6fix::BuildCompatibility::Compatible) ==
            civ6fix::PreflightDecision::RefuseExistingGame,
        "running game is refused without side effects");
    tests.Expect(
        civ6fix::DecidePreflight(civ6fix::GamePresence::Unknown,
                                 civ6fix::BuildCompatibility::Compatible) ==
            civ6fix::PreflightDecision::RefuseProcessScanFailure,
        "unavailable process scan fails closed");
    tests.Expect(
        civ6fix::DecidePreflight(civ6fix::GamePresence::NotRunning,
                                 civ6fix::BuildCompatibility::Incompatible) ==
            civ6fix::PreflightDecision::RejectBuild,
        "incompatible game build is rejected");
    tests.Expect(
        civ6fix::DecidePreflight(civ6fix::GamePresence::NotRunning,
                                 civ6fix::BuildCompatibility::Compatible) ==
            civ6fix::PreflightDecision::LaunchAndProtect,
        "compatible stopped game may be launched and protected");

    tests.Expect(
        civ6fix::DecideMonitoring({1, 4, 4, false, false}) ==
            civ6fix::MonitorDecision::DetachAndKeepHook,
        "online success detaches polling and keeps hook");
    tests.Expect(
        civ6fix::DecideMonitoring({0, 4, 4, false, false}) ==
            civ6fix::MonitorDecision::DetachOnlineWithoutIntervention,
        "naturally online state detaches polling without claiming intervention");
    tests.Expect(
        civ6fix::DecideMonitoring({1, 4, 3, false, false}) ==
            civ6fix::MonitorDecision::ContinueMonitoring,
        "monitor waits for both Discovery and SSO");
    tests.Expect(
        civ6fix::DecideMonitoring({0, 3, 3, true, false}) ==
            civ6fix::MonitorDecision::DetachTimedOutAndKeepHook,
        "timeout stops polling and keeps in-game hook");
    tests.Expect(
        civ6fix::DecideMonitoring({1, 4, 4, false, true}) ==
            civ6fix::MonitorDecision::RestoreAndStop,
        "explicit cancellation restores original IAT even at success boundary");

    constexpr std::uintptr_t expectedReturn = 0x140BCEEC5ULL;
    tests.Expect(civ6fix::ShouldSkipUnlock(expectedReturn, expectedReturn, 0),
                 "exact invalid OpenSSL unlock is skipped");
    tests.Expect(civ6fix::ShouldSkipUnlock(expectedReturn, expectedReturn, -1),
                 "already-underflowed OpenSSL unlock is skipped");
    tests.Expect(!civ6fix::ShouldSkipUnlock(expectedReturn, expectedReturn, 1),
                 "normal count=1 release is forwarded");
    tests.Expect(!civ6fix::ShouldSkipUnlock(expectedReturn + 1, expectedReturn, 0),
                 "unrelated caller is forwarded even with count=0");

    constexpr std::uint64_t counterAddress = 0x3333333344444444ULL;
    constexpr std::uint64_t originalUnlock = 0x5555555566666666ULL;
    const auto stub = civ6fix::BuildUnlockGuard(expectedReturn, counterAddress,
                                                 originalUnlock);
    tests.Expect(stub.size() == 51, "unlock guard has audited 51-byte layout");
    tests.Expect(ReadImmediate64(stub, 2) == expectedReturn,
                 "stub embeds exact callback return address");
    tests.Expect(ReadImmediate64(stub, 24) == counterAddress,
                 "stub embeds remote counter address");
    tests.Expect(ReadImmediate64(stub, 41) == originalUnlock,
                 "stub embeds original unlock address");
    tests.Expect(stub.size() > 21 && stub[14] == 0x75 && stub[15] == 0x17 &&
                      stub[20] == 0x7F && stub[21] == 0x11,
                  "both guard branches land on audited forward path");
    const auto customMutexLayoutStub = civ6fix::BuildUnlockGuard(
        expectedReturn, counterAddress, originalUnlock, 0x44);
    tests.Expect(customMutexLayoutStub.size() == 51 &&
                     customMutexLayoutStub[18] == 0x44,
                 "guard mutex count displacement comes from its Build Profile");

    return tests.Finish();
}
