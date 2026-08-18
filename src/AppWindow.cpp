#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include "AppWindow.h"

#include "Logger.h"
#include "ProductIdentity.generated.h"
#include "SessionPresentation.h"
#include "SessionStatusMailbox.h"
#include "Win32FixPlatform.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace civ6fix {
namespace {

constexpr wchar_t kWindowTitle[] = L"Civ6 2K Online Fix";
constexpr wchar_t kHeadingText[] = L"文明 VI · 2K 在线修复";
constexpr wchar_t kSafetyText[] =
    L"安全边界：仅精确 Verified 构建可写入 · DX11/DX12 自动识别 · 不修改游戏文件或账号数据";
constexpr UINT kSessionStatusMessage = WM_APP + 1;
constexpr UINT kSessionCompletedMessage = WM_APP + 2;

constexpr int kStatusId = 1001;
constexpr int kDetailId = 1002;
constexpr int kProfileId = 1003;
constexpr int kProgressId = 1004;
constexpr int kSafetyId = 1005;
constexpr int kPrimaryId = 1101;
constexpr int kSecondaryId = 1102;
constexpr int kLogsId = 1103;
constexpr int kCopyId = 1104;

HMENU ControlId(int identifier) {
    return reinterpret_cast<HMENU>(static_cast<INT_PTR>(identifier));
}

const char* PhaseName(SessionPhase phase) {
    switch (phase) {
    case SessionPhase::Created: return "created";
    case SessionPhase::Preparing: return "preparing";
    case SessionPhase::Listening: return "listening";
    case SessionPhase::WaitingForGame: return "waiting_for_game";
    case SessionPhase::ValidatingRuntime: return "validating_runtime";
    case SessionPhase::InstallingGuard: return "installing_guard";
    case SessionPhase::Monitoring: return "monitoring";
    case SessionPhase::Completed: return "completed";
    }
    return "unknown";
}

const char* ResultName(SessionResult result) {
    switch (result) {
    case SessionResult::None: return "none";
    case SessionResult::Fixed: return "fixed";
    case SessionResult::OnlineWithoutIntervention:
        return "online_without_intervention";
    case SessionResult::MonitorTimedOutHookKept:
        return "monitor_timed_out_hook_kept";
    case SessionResult::CandidateDetectedReadOnly:
        return "candidate_detected_read_only";
    case SessionResult::TargetExited: return "target_exited";
    case SessionResult::CancelledBeforeWrite: return "cancelled_before_write";
    case SessionResult::CancelledOwnedHookRestoredAllocationRetained:
        return "cancelled_owned_hook_restored_allocation_retained";
    case SessionResult::
        CancelledOwnedHookRestoredProtectionUncertainAllocationRetained:
        return "cancelled_owned_hook_restored_protection_uncertain_allocation_retained";
    case SessionResult::CancelledHookNotOwnedNoWrite:
        return "cancelled_hook_not_owned_no_write";
    case SessionResult::ExistingGameNoWrite: return "existing_game_no_write";
    case SessionResult::ProcessScanFailedNoWrite:
        return "process_scan_failed_no_write";
    case SessionResult::InstallationNotFoundNoWrite:
        return "installation_not_found_no_write";
    case SessionResult::UnsupportedBuildNoWrite:
        return "unsupported_build_no_write";
    case SessionResult::HashFailedNoWrite: return "hash_failed_no_write";
    case SessionResult::SteamLaunchFailedNoWrite:
        return "steam_launch_failed_no_write";
    case SessionResult::TargetWaitTimedOutNoWrite:
        return "target_wait_timed_out_no_write";
    case SessionResult::RuntimeMismatchNoWrite:
        return "runtime_mismatch_no_write";
    case SessionResult::GuardInstallFailed: return "guard_install_failed";
    case SessionResult::GuardInstallFailedNoHookProtectionUncertain:
        return "guard_install_failed_no_hook_protection_uncertain";
    case SessionResult::GuardInstallFailedRestoredAllocationRetained:
        return "guard_install_failed_restored_allocation_retained";
    case SessionResult::
        GuardInstallFailedRestoredProtectionUncertainAllocationRetained:
        return "guard_install_failed_restored_protection_uncertain_allocation_retained";
    case SessionResult::GuardInstallFailedStateUncertainAllocationRetained:
        return "guard_install_failed_state_uncertain_allocation_retained";
    case SessionResult::TargetWaitFailedHookKept:
        return "target_wait_failed_hook_kept";
    }
    return "unknown";
}

std::wstring ProfileText(const SessionStatus& status,
                         bool steamLaunchRequested) {
    if (status.profile == nullptr) {
        return steamLaunchRequested
                   ? L"模式：已请求 Steam 默认启动项 · 实际渲染器以进程为准"
                   : L"模式：仅监听（推荐） · 请在 Steam 选择 DX11 / DX12";
    }
    const wchar_t* renderer = status.profile->renderer == Renderer::DirectX11
                                  ? L"DirectX 11"
                                  : L"DirectX 12";
    const wchar_t* support = L"Unsupported";
    switch (status.profile->supportState) {
    case SupportState::Verified: support = L"Verified"; break;
    case SupportState::Experimental: support = L"Experimental"; break;
    case SupportState::ReadOnlyCandidate: support = L"ReadOnlyCandidate"; break;
    case SupportState::Unsupported: break;
    }
    std::wostringstream text;
    text << L"已识别：" << renderer << L" · " << support << L" · PID "
         << status.pid;
    if (status.phase == SessionPhase::Monitoring) {
        text << L" · 拦截 " << status.skippedInvalidUnlocks << L" · Discovery "
             << status.discoveryState << L" · SSO " << status.ssoState;
    }
    return text.str();
}

COLORREF ToneColor(PresentationTone tone) {
    switch (tone) {
    case PresentationTone::Neutral: return RGB(55, 65, 81);
    case PresentationTone::InProgress: return RGB(30, 94, 180);
    case PresentationTone::Success: return RGB(14, 116, 83);
    case PresentationTone::Warning: return RGB(166, 91, 0);
    case PresentationTone::Error: return RGB(185, 38, 47);
    }
    return RGB(55, 65, 81);
}

void SetClipboardText(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) {
        return;
    }
    EmptyClipboard();
    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory != nullptr) {
        void* destination = GlobalLock(memory);
        if (destination != nullptr) {
            CopyMemory(destination, text.c_str(), bytes);
            GlobalUnlock(memory);
            if (SetClipboardData(CF_UNICODETEXT, memory) != nullptr) {
                memory = nullptr;
            }
        }
    }
    if (memory != nullptr) {
        GlobalFree(memory);
    }
    CloseClipboard();
}

class WindowObserver final : public ISessionObserver {
public:
    WindowObserver(HWND window, Logger& logger, SessionStatusMailbox& mailbox)
        : window_(window), logger_(logger), mailbox_(mailbox) {}

    void OnSessionStatus(const SessionStatus& status) override {
        logger_.Event(
            "session_status",
            "\"phase\":\"" + std::string(PhaseName(status.phase)) +
                "\",\"result\":\"" + std::string(ResultName(status.result)) +
                "\",\"pid\":" + std::to_string(status.pid) +
                ",\"skipped_invalid_unlocks\":" +
                std::to_string(status.skippedInvalidUnlocks) +
                ",\"discovery\":" + std::to_string(status.discoveryState) +
                ",\"sso\":" + std::to_string(status.ssoState) +
                ",\"detail\":\"" + JsonEscape(Utf8(status.detail)) + "\"");
        mailbox_.Push(status);
        if (!PostMessageW(window_, kSessionStatusMessage, 0, 0)) {
            logger_.Event("status_wakeup_post_failed",
                          "\"win32\":" + std::to_string(GetLastError()));
        }
    }

private:
    HWND window_;
    Logger& logger_;
    SessionStatusMailbox& mailbox_;
};

class AppWindow {
public:
    AppWindow(HINSTANCE instance, Logger& logger, SessionOptions options)
        : instance_(instance), logger_(logger), options_(options),
          steamLaunchRequested_(options.launchViaSteam) {}

    ~AppWindow() {
        RequestStop();
        if (worker_.joinable()) {
            worker_.join();
        }
        DeleteObject(titleFont_);
        DeleteObject(statusFont_);
        DeleteObject(bodyFont_);
        DeleteObject(backgroundBrush_);
    }

    bool Create(int showCommand) {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = &AppWindow::WindowProc;
        windowClass.hInstance = instance_;
        windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = nullptr;
        windowClass.lpszClassName = kAppWindowClassName;
        windowClass.hIconSm = LoadIconW(nullptr, IDI_APPLICATION);
        if (RegisterClassExW(&windowClass) == 0 &&
            GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }

        const int width = 780;
        const int height = 540;
        RECT desktop{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &desktop, 0);
        const int x = desktop.left + (desktop.right - desktop.left - width) / 2;
        const int y = desktop.top + (desktop.bottom - desktop.top - height) / 2;
        window_ = CreateWindowExW(
            WS_EX_APPWINDOW, kAppWindowClassName, kWindowTitle,
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, width, height, nullptr,
            nullptr, instance_, this);
        if (window_ == nullptr) {
            return false;
        }
        const BOOL titleSet = SetWindowTextW(window_, kWindowTitle);
        ShowWindow(window_, showCommand);
        UpdateWindow(window_);
        wchar_t actualTitle[128]{};
        const int titleLength =
            GetWindowTextW(window_, actualTitle,
                           static_cast<int>(sizeof(actualTitle) /
                                            sizeof(actualTitle[0])));
        logger_.Event(
            "window_shown",
            "\"show_command\":" + std::to_string(showCommand) +
                ",\"title_set\":" +
                (titleSet ? std::string("true") : std::string("false")) +
                ",\"title_length\":" + std::to_string(titleLength) +
                ",\"visible\":" +
                (IsWindowVisible(window_) ? std::string("true")
                                          : std::string("false")) +
                ",\"win32\":" + std::to_string(GetLastError()));
        StartSession();
        return true;
    }

    HWND Handle() const noexcept { return window_; }

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam,
                                       LPARAM lParam) {
        AppWindow* self = reinterpret_cast<AppWindow*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<AppWindow*>(create->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(self));
        }
        return self != nullptr ? self->HandleMessage(message, wParam, lParam)
                               : DefWindowProcW(window, message, wParam, lParam);
    }

    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_CREATE:
            CreateControls();
            return 0;
        case WM_SIZE:
            LayoutControls(LOWORD(lParam), HIWORD(lParam));
            return 0;
        case WM_ERASEBKGND: {
            RECT area{};
            GetClientRect(window_, &area);
            FillRect(reinterpret_cast<HDC>(wParam), &area, backgroundBrush_);
            return 1;
        }
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = 680;
            info->ptMinTrackSize.y = 500;
            return 0;
        }
        case WM_COMMAND:
            HandleCommand(LOWORD(wParam));
            return 0;
        case WM_CLOSE:
            if (workerRunning_) {
                closingRequested_ = true;
                RequestStop();
                SetWindowTextW(primaryButton_, L"正在安全退出…");
                EnableWindow(primaryButton_, FALSE);
                return 0;
            }
            DestroyWindow(window_);
            return 0;
        case WM_CTLCOLORSTATIC: {
            HDC device = reinterpret_cast<HDC>(wParam);
            SetBkMode(device, TRANSPARENT);
            if (reinterpret_cast<HWND>(lParam) == status_) {
                SetTextColor(device, ToneColor(tone_));
            } else if (reinterpret_cast<HWND>(lParam) == profile_ ||
                       reinterpret_cast<HWND>(lParam) == safety_) {
                SetTextColor(device, RGB(92, 103, 120));
            } else {
                SetTextColor(device, RGB(35, 42, 54));
            }
            return reinterpret_cast<LRESULT>(backgroundBrush_);
        }
        case kSessionStatusMessage: {
            DrainStatusMailbox();
            return 0;
        }
        case kSessionCompletedMessage:
            if (!workerFinished_.exchange(false, std::memory_order_acq_rel)) {
                return 0;
            }
            workerRunning_ = false;
            if (worker_.joinable()) {
                worker_.join();
            }
            ApplySessionControls(latestStatus_);
            SendMessageW(progress_, PBM_SETMARQUEE, FALSE, 0);
            ShowWindow(progress_, SW_HIDE);
            if (closingRequested_) {
                DestroyWindow(window_);
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window_, message, wParam, lParam);
        }
    }

    void CreateControls() {
        backgroundBrush_ = CreateSolidBrush(RGB(247, 249, 252));
        titleFont_ = CreateFontW(-29, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH, L"Segoe UI");
        statusFont_ = CreateFontW(-23, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE,
                                  FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                  CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                  DEFAULT_PITCH, L"Microsoft YaHei UI");
        bodyFont_ = CreateFontW(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH, L"Microsoft YaHei UI");

        title_ = CreateWindowExW(0, L"STATIC", kHeadingText,
                                 WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
                                 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
        status_ = CreateWindowExW(0, L"STATIC", L"正在准备安全会话…",
                                  WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
                                  0, 0, 0, 0, window_,
                                  ControlId(kStatusId), instance_,
                                  nullptr);
        detail_ = CreateWindowExW(0, L"STATIC",
                                  L"界面不会阻塞监听；无需点击确认。",
                                  WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
                                  0, 0, 0, 0, window_,
                                  ControlId(kDetailId), instance_,
                                  nullptr);
        profile_ = CreateWindowExW(
            0, L"STATIC", ProfileText({}, steamLaunchRequested_).c_str(),
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 0, 0, window_,
            ControlId(kProfileId), instance_, nullptr);
        progress_ = CreateWindowExW(
            0, PROGRESS_CLASSW, nullptr,
            WS_CHILD | WS_VISIBLE | PBS_MARQUEE | PBS_SMOOTH, 0, 0, 0, 0,
            window_, ControlId(kProgressId), instance_, nullptr);
        safety_ = CreateWindowExW(
            0, L"STATIC", kSafetyText,
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 0, 0, window_,
            ControlId(kSafetyId), instance_, nullptr);
        primaryButton_ = CreateWindowExW(
            0, L"BUTTON", L"取消本次", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                              BS_PUSHBUTTON,
            0, 0, 0, 0, window_, ControlId(kPrimaryId), instance_,
            nullptr);
        secondaryButton_ = CreateWindowExW(
            0, L"BUTTON", L"监听建立后可启动",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 0, 0, window_, ControlId(kSecondaryId), instance_,
            nullptr);
        logsButton_ = CreateWindowExW(
            0, L"BUTTON", L"打开日志", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                            BS_PUSHBUTTON,
            0, 0, 0, 0, window_, ControlId(kLogsId), instance_,
            nullptr);
        copyButton_ = CreateWindowExW(
            0, L"BUTTON", L"复制脱敏诊断", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                            BS_PUSHBUTTON,
            0, 0, 0, 0, window_, ControlId(kCopyId), instance_,
            nullptr);

        SendMessageW(title_, WM_SETFONT, reinterpret_cast<WPARAM>(titleFont_), TRUE);
        SendMessageW(status_, WM_SETFONT, reinterpret_cast<WPARAM>(statusFont_), TRUE);
        for (HWND control : {detail_, profile_, safety_, primaryButton_, secondaryButton_,
                             logsButton_, copyButton_}) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(bodyFont_),
                         TRUE);
        }
        EnableWindow(secondaryButton_, FALSE);
        SendMessageW(progress_, PBM_SETMARQUEE, TRUE, 35);
    }

    void LayoutControls(int width, int height) {
        const int margin = 34;
        const int contentWidth = std::max(100, width - margin * 2);
        MoveWindow(title_, margin, 25, contentWidth, 42, TRUE);
        MoveWindow(status_, margin, 82, contentWidth, 34, TRUE);
        MoveWindow(detail_, margin, 126, contentWidth, 90, TRUE);
        MoveWindow(profile_, margin, 228, contentWidth, 28, TRUE);
        MoveWindow(progress_, margin, 270, contentWidth, 7, TRUE);
        MoveWindow(safety_, margin, 305, contentWidth, 56, TRUE);

        const int buttonWidth = 116;
        const int primaryButtonWidth = 176;
        const int buttonHeight = 38;
        const int gap = 10;
        const int buttonY = std::max(380, height - margin - buttonHeight - 12);
        const int rightButtonsX =
            width - margin - buttonWidth * 2 - gap;
        const int secondaryX = margin + primaryButtonWidth + gap;
        const int secondaryWidth =
            std::max(116, rightButtonsX - gap - secondaryX);
        MoveWindow(primaryButton_, margin, buttonY, primaryButtonWidth,
                   buttonHeight, TRUE);
        MoveWindow(secondaryButton_, secondaryX, buttonY, secondaryWidth,
                   buttonHeight, TRUE);
        MoveWindow(copyButton_, rightButtonsX, buttonY,
                   buttonWidth, buttonHeight, TRUE);
        MoveWindow(logsButton_, width - margin - buttonWidth, buttonY, buttonWidth,
                   buttonHeight, TRUE);
    }

    void HandleCommand(int identifier) {
        switch (identifier) {
        case kPrimaryId:
            if (primaryAction_ == SessionPrimaryAction::CloseWindow) {
                logger_.Event("window_primary_action", "\"action\":\"close_window\"");
                if (workerRunning_) {
                    closingRequested_ = true;
                    RequestStop();
                    SetWindowTextW(primaryButton_, L"正在安全退出…");
                    EnableWindow(primaryButton_, FALSE);
                } else {
                    DestroyWindow(window_);
                }
                break;
            }
            logger_.Event("window_primary_action", "\"action\":\"request_stop\"");
            RequestStop();
            SetWindowTextW(primaryButton_, L"正在安全停止…");
            EnableWindow(primaryButton_, FALSE);
            break;
        case kSecondaryId:
            if (secondaryAction_ == SessionLaunchAction::RequestSteamDefault) {
                logger_.Event("window_secondary_action",
                              "\"action\":\"request_steam_default\"");
                steamLaunchRequested_ = true;
                RequestSteamLaunch();
                ApplySessionLaunchControl(latestStatus_);
                const std::wstring mode =
                    ProfileText(latestStatus_, steamLaunchRequested_);
                SetWindowTextW(profile_, mode.c_str());
                break;
            }
            if (secondaryAction_ == SessionLaunchAction::RetrySession) {
                logger_.Event("window_secondary_action",
                              "\"action\":\"retry_session\"");
                StartSession();
            }
            break;
        case kLogsId: {
            const std::filesystem::path folder = logger_.Path().parent_path();
            ShellExecuteW(window_, L"open", folder.c_str(), nullptr, nullptr,
                          SW_SHOWNORMAL);
            break;
        }
        case kCopyId: {
            SetClipboardText(
                window_, BuildShareableDiagnostic(
                             kProductVersionWide, latestStatus_,
                             steamLaunchRequested_));
            SetWindowTextW(copyButton_, L"已复制");
            break;
        }
        default:
            break;
        }
    }

    void ApplyStatus(const SessionStatus& status) {
        latestStatus_ = status;
        latestPresentation_ = PresentSessionStatus(status);
        ApplySessionControls(status);
        tone_ = latestPresentation_.tone;
        SetWindowTextW(status_, latestPresentation_.headline.c_str());
        SetWindowTextW(detail_, latestPresentation_.detail.c_str());
        const std::wstring profile =
            ProfileText(status, steamLaunchRequested_);
        SetWindowTextW(profile_, profile.c_str());
        InvalidateRect(status_, nullptr, TRUE);
        InvalidateRect(profile_, nullptr, TRUE);
        if (!latestPresentation_.terminal) {
            ShowWindow(progress_, SW_SHOW);
            SendMessageW(progress_, PBM_SETMARQUEE, TRUE, 35);
        }
    }

    void ApplySessionControls(const SessionStatus& status) {
        const SessionControls controls = PresentSessionControls(status);
        primaryAction_ = controls.primaryAction;
        SetWindowTextW(primaryButton_, controls.primaryLabel.c_str());
        EnableWindow(primaryButton_, controls.primaryEnabled ? TRUE : FALSE);
        ApplySessionLaunchControl(status);
    }

    void ApplySessionLaunchControl(const SessionStatus& status) {
        const SessionLaunchControl control =
            PresentSessionLaunchControl(status, steamLaunchRequested_);
        secondaryAction_ = control.action;
        SetWindowTextW(secondaryButton_, control.label.c_str());
        EnableWindow(secondaryButton_, control.enabled ? TRUE : FALSE);
        ShowWindow(secondaryButton_, control.visible ? SW_SHOW : SW_HIDE);
    }

    void DrainStatusMailbox() {
        for (;;) {
            std::optional<SessionStatus> status = statusMailbox_.TryPop();
            if (!status.has_value()) {
                return;
            }
            ApplyStatus(*status);
        }
    }

    void StartSession() {
        if (workerRunning_) {
            return;
        }
        if (worker_.joinable()) {
            worker_.join();
        }
        closingRequested_ = false;
        steamLaunchRequested_ = options_.launchViaSteam;
        {
            std::lock_guard<std::mutex> lock(sessionMutex_);
            stopPending_ = false;
            steamLaunchPending_ = false;
            activeSession_ = nullptr;
        }
        workerRunning_ = true;
        workerFinished_.store(false, std::memory_order_release);
        SessionStatus startingStatus;
        startingStatus.phase = SessionPhase::Preparing;
        ApplySessionControls(startingStatus);
        SetWindowTextW(copyButton_, L"复制脱敏诊断");
        ShowWindow(progress_, SW_SHOW);
        SendMessageW(progress_, PBM_SETMARQUEE, TRUE, 35);

        worker_ = std::thread([this]() {
            Win32FixPlatform platform(logger_);
            WindowObserver observer(window_, logger_, statusMailbox_);
            FixSession session(platform, observer, options_);
            {
                std::lock_guard<std::mutex> lock(sessionMutex_);
                activeSession_ = &session;
                if (stopPending_) {
                    session.RequestStop();
                }
                if (steamLaunchPending_) {
                    session.RequestSteamLaunch();
                }
            }
            const SessionResult result = session.Run();
            {
                std::lock_guard<std::mutex> lock(sessionMutex_);
                activeSession_ = nullptr;
            }
            workerFinished_.store(true, std::memory_order_release);
            PostMessageW(window_, kSessionCompletedMessage,
                         static_cast<WPARAM>(result), 0);
        });
    }

    void RequestStop() {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        stopPending_ = true;
        if (activeSession_ != nullptr) {
            activeSession_->RequestStop();
        }
    }

    void RequestSteamLaunch() {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        steamLaunchPending_ = true;
        if (activeSession_ != nullptr) {
            activeSession_->RequestSteamLaunch();
        }
    }

    HINSTANCE instance_ = nullptr;
    Logger& logger_;
    SessionOptions options_;
    HWND window_ = nullptr;
    HWND title_ = nullptr;
    HWND status_ = nullptr;
    HWND detail_ = nullptr;
    HWND profile_ = nullptr;
    HWND progress_ = nullptr;
    HWND safety_ = nullptr;
    HWND primaryButton_ = nullptr;
    HWND secondaryButton_ = nullptr;
    HWND logsButton_ = nullptr;
    HWND copyButton_ = nullptr;
    HFONT titleFont_ = nullptr;
    HFONT statusFont_ = nullptr;
    HFONT bodyFont_ = nullptr;
    HBRUSH backgroundBrush_ = nullptr;
    PresentationTone tone_ = PresentationTone::InProgress;
    SessionPrimaryAction primaryAction_ = SessionPrimaryAction::RequestStop;
    SessionLaunchAction secondaryAction_ = SessionLaunchAction::None;
    SessionStatus latestStatus_;
    SessionPresentation latestPresentation_;
    SessionStatusMailbox statusMailbox_;
    std::thread worker_;
    std::atomic_bool workerFinished_{false};
    std::mutex sessionMutex_;
    FixSession* activeSession_ = nullptr;
    bool stopPending_ = false;
    bool steamLaunchPending_ = false;
    bool steamLaunchRequested_ = false;
    bool workerRunning_ = false;
    bool closingRequested_ = false;
};

}  // namespace

bool ActivateExistingAppWindow() {
    HWND window = FindWindowW(kAppWindowClassName, nullptr);
    if (window == nullptr) {
        return false;
    }
    if (IsIconic(window)) {
        ShowWindow(window, SW_RESTORE);
    } else {
        ShowWindow(window, SW_SHOW);
    }
    SetForegroundWindow(window);
    FlashWindow(window, TRUE);
    return true;
}

int RunAppWindow(HINSTANCE instance, int showCommand, Logger& logger,
                 SessionOptions options) {
    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&controls);

    AppWindow window(instance, logger, options);
    if (!window.Create(showCommand)) {
        logger.Event("window_create_failed",
                     "\"win32\":" + std::to_string(GetLastError()));
        return 4;
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window.Handle(), &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return static_cast<int>(message.wParam);
}

}  // namespace civ6fix
