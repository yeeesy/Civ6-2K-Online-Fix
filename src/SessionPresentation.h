#pragma once

#include "FixSession.h"

#include <string>
#include <string_view>

namespace civ6fix {

enum class PresentationTone {
    Neutral,
    InProgress,
    Success,
    Warning,
    Error,
};

struct SessionPresentation {
    std::wstring headline;
    std::wstring detail;
    PresentationTone tone = PresentationTone::Neutral;
    bool terminal = false;
    bool retryRecommended = false;
};

enum class SessionPrimaryAction {
    RequestStop,
    CloseWindow,
};

struct SessionControls {
    std::wstring primaryLabel;
    SessionPrimaryAction primaryAction = SessionPrimaryAction::RequestStop;
    bool primaryEnabled = true;
};

enum class SessionLaunchAction {
    None,
    RequestSteamDefault,
    RetrySession,
};

struct SessionLaunchControl {
    std::wstring label;
    SessionLaunchAction action = SessionLaunchAction::None;
    bool enabled = false;
    bool visible = false;
};

SessionPresentation PresentSessionResult(SessionResult result);
SessionPresentation PresentSessionStatus(const SessionStatus& status);
SessionControls PresentSessionControls(const SessionStatus& status);
SessionLaunchControl PresentSessionLaunchControl(
    const SessionStatus& status, bool steamLaunchRequested);
std::wstring BuildShareableDiagnostic(
    std::wstring_view productVersion, const SessionStatus& status,
    bool steamLaunchRequested);

}  // namespace civ6fix
