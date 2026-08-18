#pragma once

#include "FixSession.h"

#include <memory>

namespace civ6fix {

class Logger;

class Win32FixPlatform final : public IFixSessionPlatform {
public:
    explicit Win32FixPlatform(Logger& logger);
    ~Win32FixPlatform() override;

    Win32FixPlatform(const Win32FixPlatform&) = delete;
    Win32FixPlatform& operator=(const Win32FixPlatform&) = delete;

    PreparationResult Prepare() override;
    bool LaunchViaSteam(std::wstring& detail) override;
    TargetScanResult ScanForTarget() override;
    RuntimeValidationResult ValidateRuntime(
        const TargetProcess& target, const BuildProfile& profile) override;
    GuardInstallResult InstallGuard(const TargetProcess& target,
                                     const BuildProfile& profile) override;
    TargetRunState QueryTargetState(const TargetProcess& target) override;
    MonitorSample ReadMonitorSample(const TargetProcess& target,
                                     const InstalledGuard& guard) override;
    HookCleanupOutcome RestoreOwnedHook(
        const TargetProcess& target, const InstalledGuard& guard) override;
    std::uint64_t MonotonicMilliseconds() override;
    void SleepFor(std::uint32_t milliseconds) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace civ6fix
