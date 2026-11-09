#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include "AppWindow.h"
#include "Civ6FixCore.h"
#include "FixSession.h"
#include "Logger.h"
#include "ProductIdentity.generated.h"
#include "SessionPresentation.h"
#include "SessionStatusMailbox.h"
#include "SteamDiscovery.h"
#include "Win32FixPlatform.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr wchar_t kInstanceMutexName[] =
    L"Local\\Civ6_2K_Online_Fix_v2_Session";

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE handle = nullptr) : handle_(handle) {}
    ~ScopedHandle() {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

private:
    HANDLE handle_ = nullptr;
};

void EnsureParentConsole() {
    if (GetConsoleWindow() == nullptr) {
        AttachConsole(ATTACH_PARENT_PROCESS);
    }
}

void ConsoleLine(std::wstring_view text) {
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (output == nullptr || output == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD consoleMode = 0;
    if (GetConsoleMode(output, &consoleMode)) {
        DWORD written = 0;
        WriteConsoleW(output, text.data(), static_cast<DWORD>(text.size()),
                      &written, nullptr);
        static constexpr wchar_t newline[] = L"\r\n";
        WriteConsoleW(output, newline, 2, &written, nullptr);
        return;
    }
    std::string utf8 = civ6fix::Utf8(text);
    utf8 += "\r\n";
    DWORD written = 0;
    WriteFile(output, utf8.data(), static_cast<DWORD>(utf8.size()), &written,
              nullptr);
}

class ConsoleObserver final : public civ6fix::ISessionObserver {
public:
    void OnSessionStatus(const civ6fix::SessionStatus& status) override {
        const auto presentation = civ6fix::PresentSessionStatus(status);
        ConsoleLine(presentation.headline + L"：" + presentation.detail);
    }
};

struct CommandLineOptions {
    bool selfTest = false;
    bool diagnose = false;
    bool observeCandidate = false;
    bool watchOnly = false;
    bool autoLaunchSteam = false;
    bool noGui = false;
    bool help = false;
    bool version = false;
    bool profiles = false;
    bool valid = true;
    std::wstring invalidArgument;
};

CommandLineOptions ParseCommandLine() {
    CommandLineOptions options;
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (arguments == nullptr) {
        options.valid = false;
        options.invalidArgument = L"<CommandLineToArgvW failed>";
        return options;
    }
    for (int index = 1; index < count; ++index) {
        const std::wstring_view argument(arguments[index]);
        if (argument == L"--self-test") {
            options.selfTest = true;
        } else if (argument == L"--diagnose") {
            options.diagnose = true;
        } else if (argument == L"--observe-candidate") {
            options.observeCandidate = true;
        } else if (argument == L"--watch-only") {
            options.watchOnly = true;
        } else if (argument == L"--auto-launch-steam") {
            options.autoLaunchSteam = true;
        } else if (argument == L"--no-gui") {
            options.noGui = true;
        } else if (argument == L"--help" || argument == L"-h") {
            options.help = true;
        } else if (argument == L"--version") {
            options.version = true;
        } else if (argument == L"--print-profiles") {
            options.profiles = true;
        } else {
            options.valid = false;
            options.invalidArgument.assign(argument);
            break;
        }
    }
    LocalFree(arguments);

    const int primaryModes = static_cast<int>(options.selfTest) +
                             static_cast<int>(options.diagnose) +
                             static_cast<int>(options.help) +
                             static_cast<int>(options.version) + static_cast<int>(options.profiles) +
                             static_cast<int>(options.observeCandidate);
    if (primaryModes > 1 ||
        ((options.selfTest || options.diagnose || options.help ||
          options.version || options.profiles || options.observeCandidate) &&
         (options.watchOnly || options.autoLaunchSteam || options.noGui)) ||
        (options.watchOnly && options.autoLaunchSteam)) {
        options.valid = false;
        options.invalidArgument = L"互斥的运行模式";
    }
    return options;
}

void PrintHelp() {
    ConsoleLine(civ6fix::kProductVersionWide);
    ConsoleLine(L"用法：Civ6_2K_Online_Fix.exe [选项]");
    ConsoleLine(L"  无参数              打开 GUI，只监听；推荐在 Steam 手动选择 DX11/DX12");
    ConsoleLine(L"  --watch-only        只监听，由用户自行启动 Steam（默认行为）");
    ConsoleLine(L"  --auto-launch-steam 显式请求 Steam 默认启动项；不保证渲染器");
    ConsoleLine(L"  --no-gui            无窗口运行（可与上述启动模式组合）");
    ConsoleLine(L"  --diagnose          只读检查 Steam 安装、进程和 Build Profile");
    ConsoleLine(L"  --observe-candidate 维护者：监听只读候选，输出服务状态变化，不启动游戏");
    ConsoleLine(L"  --self-test         纯离线核心自检，不枚举或打开游戏进程");
    ConsoleLine(L"  --version           显示版本");
    ConsoleLine(L"  --help, -h          显示本帮助");
}

bool ConfiguredAutoLaunchEnabled() {
    std::vector<wchar_t> executable(32768);
    const DWORD length = GetModuleFileNameW(
        nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size()) {
        return false;
    }
    std::filesystem::path settings(
        std::wstring(executable.data(), static_cast<std::size_t>(length)));
    settings.replace_extension(L".ini");
    return GetPrivateProfileIntW(L"launch", L"auto_launch_steam", 0,
                                 settings.c_str()) != 0;
}

int RunOfflineSelfTest(civ6fix::Logger& logger) {
    logger.Event("offline_self_test_started",
                 "\"process_enumeration\":false,\"process_write\":false");
    const auto* dx11 = civ6fix::FindBuildProfile(
        L"CivilizationVI.exe",
        L"E7450823CC8E00468CFF7B9D7B97C63140EAE38AE1D774BA4EFA437556C42D63");
    const auto* dx12 = civ6fix::FindBuildProfile(
        L"CivilizationVI_DX12.exe",
        L"C2C3D40B86260A541D8A4D38CB70D50D3406AE1DE374AFC382D1E42BC1342F1E");
    const auto stub = civ6fix::BuildUnlockGuard(0x1000, 0x2000, 0x3000);
    const auto libraries = civ6fix::ParseSteamLibraryFolders(
        R"VDF("libraryfolders" { "0" { "path" "T:\\SteamLibrary" } })VDF");
    const auto installDir = civ6fix::ParseSteamAppInstallDir(
        R"VDF("AppState" { "appid" "289070" "installdir" "Civilization VI" })VDF");
    civ6fix::SessionStatusMailbox mailbox;
    const bool dx12PolicyValid =
        dx12 != nullptr &&
        dx12->supportState == civ6fix::SupportState::Verified &&
        civ6fix::DecideBuildUse(dx12) ==
            civ6fix::BuildUseDecision::InstallVerifiedGuard;
    const bool passed =
        dx11 != nullptr &&
        dx11->supportState == civ6fix::SupportState::Verified &&
        dx12PolicyValid &&
        stub.size() == 51 && libraries.size() == 1 &&
        installDir == std::optional<std::wstring>(L"Civilization VI") &&
        !mailbox.TryPop().has_value() &&
        civ6fix::DecideRuntimeFileIdentity({true, true, true, true}) ==
            civ6fix::RuntimeFileIdentityDecision::AcceptExactIdentity &&
        civ6fix::EvaluatePointerPatch({true, true, true, true, true}) ==
            civ6fix::PointerPatchOutcome::Succeeded &&
        !civ6fix::PlanHookCleanup(true,
                                  civ6fix::IatOwnership::OwnedBySession)
             .freeRemoteAllocation;
    logger.Event("offline_self_test_completed",
                 "\"passed\":" +
                     std::string(passed ? "true" : "false"));
    ConsoleLine(passed ? L"离线自检通过。" : L"离线自检失败，请查看日志。");
    return passed ? 0 : 2;
}

int RunDiagnosis(civ6fix::Logger& logger) {
    logger.Event("read_only_diagnosis_started", "\"writes\":false");
    civ6fix::Win32FixPlatform platform(logger);
    const civ6fix::PreparationResult result = platform.Prepare();
    const bool ready = result.outcome == civ6fix::PreparationOutcome::Ready;
    ConsoleLine(ready ? L"只读诊断通过：至少一个已知 Build Profile 可用。"
                      : L"只读诊断未通过：" + result.detail);
    logger.Event("read_only_diagnosis_completed",
                 "\"ready\":" +
                     std::string(ready ? "true" : "false") +
                     ",\"detail\":\"" +
                     civ6fix::JsonEscape(civ6fix::Utf8(result.detail)) + "\"");
    return ready ? 0 : 6;
}

int RunCandidateObservation(civ6fix::Logger& logger) {
    using namespace civ6fix;
    Win32FixPlatform platform(logger);
    const auto prep=platform.Prepare();
    if(prep.outcome!=PreparationOutcome::Ready){ConsoleLine(L"预检未就绪。");return 6;}
    ConsoleLine(L"只读监听已启动。请从 Steam 启动要验证的渲染模式。");
    TargetProcess target;
    const auto wait=platform.MonotonicMilliseconds();
    while(platform.MonotonicMilliseconds()-wait<300000) {
        const auto scan=platform.ScanForTarget();
        if(scan.outcome==TargetScanOutcome::Failed){ConsoleLine(L"进程检测失败。");return 6;}
        if(scan.outcome==TargetScanOutcome::Found){target=scan.target;break;}
        platform.SleepFor(25);
    }
    if(!target.profile || target.profile->supportState!=SupportState::ReadOnlyCandidate) {
        ConsoleLine(L"未检测到已登记的只读候选。");return 6;
    }
    const auto valid=platform.ValidateRuntime(target,*target.profile);
    if(valid.outcome!=RuntimeValidationOutcome::Valid) {
        ConsoleLine(L"运行时校验未通过；Win32="+std::to_wstring(valid.win32Error));return 6;
    }
    ConsoleLine(target.profile->renderer==Renderer::DirectX11?L"DX11 只读观察":L"DX12 只读观察");
    const auto start=platform.MonotonicMilliseconds();
    MonitorSample last;
    std::uint64_t samples=0,ready=0,negative=0;
    bool waitFailed=false;
    while(platform.MonotonicMilliseconds()-start<90000) {
        const auto running=platform.QueryTargetState(target);
        if(running!=TargetRunState::Running){waitFailed=running==TargetRunState::WaitFailed;break;}
        std::optional<std::int32_t> count;
        const auto m=platform.ObserveCandidate(target,count);
        if(samples==0 || !SessionDiagnostics::SameSample(last,m)) {
            const auto field=[](FieldRead f,int n) {
                if(f.quality==ReadQuality::Value)return std::to_wstring(n);
                if(f.quality==ReadQuality::NotCreated)return std::wstring(L"空指针");
                return L"未读取（Win32="+std::to_wstring(f.error)+L"）";
            };
            ConsoleLine(L"+"+std::to_wstring(platform.MonotonicMilliseconds()-start)+L"ms Discovery="+
                        field(m.discovery,m.discoveryState)+L" SSO="+field(m.sso,m.ssoState));
        }
        ++samples; last=m;
        if(m.discovery.quality==ReadQuality::Value && m.sso.quality==ReadQuality::Value &&
           m.discoveryState==4 && m.ssoState==4) ++ready;
        if(count && *count<0) ++negative;
        platform.SleepFor(25);
    }
    ConsoleLine(L"只读样本="+std::to_wstring(samples)+L"；4/4 样本="+std::to_wstring(ready)+
                L"；负锁计数样本="+std::to_wstring(negative));
    return !samples || waitFailed ? 6 : 0; // Exit 0 means observations collected, not repair.
}

int RunHeadless(civ6fix::Logger& logger, bool autoLaunchSteam) {
    ConsoleObserver observer;
    civ6fix::Win32FixPlatform platform(logger);
    civ6fix::SessionOptions options;
    options.launchViaSteam = autoLaunchSteam;
    civ6fix::FixSession session(platform, observer, options);
    return civ6fix::ExitCodeForSessionResult(session.Run());
}

}  // namespace

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE previousInstance,
                    _In_ PWSTR rawCommandLine, _In_ int showCommand) {
    (void)previousInstance;
    (void)rawCommandLine;
    const CommandLineOptions commandLine = ParseCommandLine();
    const bool consoleMode = commandLine.selfTest || commandLine.diagnose ||
                             commandLine.noGui || commandLine.help ||
                             commandLine.version || commandLine.profiles || commandLine.observeCandidate || !commandLine.valid;
    if (consoleMode) {
        EnsureParentConsole();
    }
    if (!commandLine.valid) {
        ConsoleLine(L"不支持或互斥的参数：" + commandLine.invalidArgument);
        PrintHelp();
        return 3;
    }
    if (commandLine.help) {
        PrintHelp();
        return 0;
    }
    if (commandLine.version) {
        ConsoleLine(civ6fix::kProductVersionWide);
        return 0;
    }
    if (commandLine.profiles) {
        std::wostringstream out;
        out << L"[";
        bool first=true;
        for (const auto* p:civ6fix::kKnownBuildProfiles) {
            if (!first) out << L",";
            first=false;
            out << L"{\"Id\":\"" << p->id << L"\",\"Renderer\":\"" <<
                (p->renderer==civ6fix::Renderer::DirectX11?L"DX11":L"DX12") <<
                L"\",\"SupportState\":\"" << (p->supportState==civ6fix::SupportState::Verified?L"Verified":
                p->supportState==civ6fix::SupportState::ReadOnlyCandidate?L"ReadOnlyCandidate":
                p->supportState==civ6fix::SupportState::Experimental?L"Experimental":L"Unsupported") <<
                L"\",\"SHA256\":\"" << p->sha256 << L"\",\"FileSize\":" << p->fileSize <<
                L",\"PeTimestamp\":" << p->peTimestamp << L",\"ImageSize\":" << p->imageSize <<
                L",\"UnlockIat\":" << p->mtxUnlockIatRva << L",\"UnlockReturn\":" << p->opensslUnlockReturnRva <<
                L",\"LockVector\":" << p->opensslLockVectorRva << L",\"Discovery\":" << p->discoveryGlobalRva <<
                L",\"SSO\":" << p->ssoGlobalRva << L",\"OwnerOffset\":" << p->mutexOwnerOffset <<
                L",\"CountOffset\":" << p->mutexCountOffset << L",\"LockIndex\":" << p->opensslLockIndex <<
                L",\"PreferredBase\":" << p->preferredImageBase << L",\"DllCharacteristics\":" << p->dllCharacteristics <<
                L",\"Executable\":\"" << p->executableName << L"\",\"CheckUnlockCall\":" << (p->requireDirectUnlockCall ? L"true" : L"false") << L"}";
        }
        ConsoleLine(out.str()+L"]");
        return 0;
    }

    std::unique_ptr<ScopedHandle> instanceMutex;
    if (!commandLine.selfTest && !commandLine.diagnose) {
        HANDLE mutex = CreateMutexW(nullptr, FALSE, kInstanceMutexName);
        if (mutex == nullptr) {
            ConsoleLine(L"无法建立单实例锁；为安全起见没有继续。");
            return 7;
        }
        const bool alreadyRunning = GetLastError() == ERROR_ALREADY_EXISTS;
        instanceMutex = std::make_unique<ScopedHandle>(mutex);
        if (alreadyRunning) {
            if (!commandLine.noGui && !commandLine.observeCandidate) {
                for (int attempt = 0; attempt < 40; ++attempt) {
                    if (civ6fix::ActivateExistingAppWindow()) {
                        break;
                    }
                    Sleep(50);
                }
            } else {
                ConsoleLine(L"已有一个修复会话正在运行；本实例未执行任何操作。");
            }
            return civ6fix::ExitCodeForExistingInstance(commandLine.noGui || commandLine.observeCandidate);
        }
    }

    civ6fix::Logger logger;
    logger.SetConsoleEcho(!commandLine.observeCandidate);
    if (!logger.Open()) {
        if (consoleMode) {
            ConsoleLine(L"无法创建日志；为安全起见没有继续。");
        } else {
            MessageBoxW(nullptr, L"无法创建日志；为安全起见没有继续。",
                        L"Civ6 2K Online Fix", MB_OK | MB_ICONERROR);
        }
        return 1;
    }
    logger.Event("application_started",
                 "\"version\":\"" + std::string(civ6fix::kProductVersion) +
                     "\",\"mode\":\"" +
                     (commandLine.selfTest
                          ? std::string("self_test")
                          : (commandLine.diagnose
                                 ? std::string("diagnose")
                                 : (commandLine.noGui
                                        ? std::string("headless")
                                        : std::string("gui")))) +
                     "\"");

    if (commandLine.selfTest) {
        return RunOfflineSelfTest(logger);
    }
    if (commandLine.observeCandidate) return RunCandidateObservation(logger);
    if (commandLine.diagnose) {
        return RunDiagnosis(logger);
    }
    const bool autoLaunchSteam =
        commandLine.autoLaunchSteam ||
        (!commandLine.watchOnly && ConfiguredAutoLaunchEnabled());
    if (commandLine.noGui) {
        return RunHeadless(logger, autoLaunchSteam);
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    civ6fix::SessionOptions options;
    options.launchViaSteam = autoLaunchSteam;
    return civ6fix::RunAppWindow(instance, showCommand, logger, options);
}
