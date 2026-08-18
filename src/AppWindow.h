#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "FixSession.h"

namespace civ6fix {

class Logger;

inline constexpr wchar_t kAppWindowClassName[] =
    L"Civ6_2K_Online_Fix_v2_Window";

bool ActivateExistingAppWindow();
int RunAppWindow(HINSTANCE instance, int showCommand, Logger& logger,
                 SessionOptions options);

}  // namespace civ6fix
