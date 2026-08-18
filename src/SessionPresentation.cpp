#include "SessionPresentation.h"

#include <sstream>

namespace civ6fix {
namespace {

const wchar_t* RendererName(Renderer renderer) noexcept {
    return renderer == Renderer::DirectX11 ? L"DirectX 11" : L"DirectX 12";
}

const wchar_t* SupportStateName(SupportState state) noexcept {
    switch (state) {
    case SupportState::Verified: return L"Verified";
    case SupportState::Experimental: return L"Experimental";
    case SupportState::ReadOnlyCandidate: return L"ReadOnlyCandidate";
    case SupportState::Unsupported: return L"Unsupported";
    }
    return L"Unsupported";
}

}  // namespace

SessionPresentation PresentSessionResult(SessionResult result) {
    switch (result) {
    case SessionResult::None:
        return {L"准备就绪", L"正在建立安全会话。",
                PresentationTone::Neutral, false, false};
    case SessionResult::Fixed:
        return {L"2K 在线初始化已修复",
                L"已拦截目标异常解锁，Discovery 与 SSO 均已就绪。修复已完成，无需保持工具窗口；现在可以关闭窗口继续游戏。轻量保护会保留在当前游戏进程中，并在游戏退出时自动消失。",
                PresentationTone::Success, true, false};
    case SessionResult::OnlineWithoutIntervention:
        return {L"2K 已自然上线",
                L"本轮没有观察到需要拦截的异常。无需保持工具窗口，现在可以关闭窗口继续游戏；保护会在游戏退出时自动消失。",
                PresentationTone::Success, true, false};
    case SessionResult::MonitorTimedOutHookKept:
        return {L"状态确认超时",
                L"高频监控已停止；为避免中途失去保护，Hook Guard 会保留到游戏退出。现在可以关闭窗口，详细状态可在日志中查看。",
                PresentationTone::Warning, true, true};
    case SessionResult::CandidateDetectedReadOnly:
        return {L"识别到只读候选构建",
                L"该 Profile 只允许诊断，因此未写入游戏进程。",
                PresentationTone::Warning, true, false};
    case SessionResult::TargetExited:
        return {L"游戏已经退出",
                L"目标进程退出后，Windows 已回收本局 Hook Guard 内存。本次已经结束，点击“关闭窗口”即可退出工具。",
                PresentationTone::Neutral, true, false};
    case SessionResult::CancelledBeforeWrite:
        return {L"本次操作已取消", L"取消发生在写入之前；游戏进程未被修改。",
                PresentationTone::Neutral, true, true};
    case SessionResult::CancelledOwnedHookRestoredAllocationRetained:
        return {L"保护已安全停止",
                L"只恢复了本会话仍拥有的 IAT；远程代码不热释放，将由游戏退出时回收。",
                PresentationTone::Neutral, true, false};
    case SessionResult::
        CancelledOwnedHookRestoredProtectionUncertainAllocationRetained:
        return {L"IAT 已恢复，页面保护状态需复核",
                L"最终回读确认 IAT 已恢复为原始目标，但页面保护恢复没有完整成功。远程内存保留到游戏退出；请结束本局后再重试。",
                PresentationTone::Warning, true, false};
    case SessionResult::CancelledHookNotOwnedNoWrite:
        return {L"停止时发现所有权变化",
                L"IAT 已不是本会话所有，因此没有覆盖它；远程内存保留到游戏退出。",
                PresentationTone::Warning, true, true};
    case SessionResult::ExistingGameNoWrite:
        return {L"文明 VI 已经运行",
                L"为保护当前游戏，本工具没有附加或写入。退出游戏后再重新运行。",
                PresentationTone::Warning, true, true};
    case SessionResult::ProcessScanFailedNoWrite:
        return {L"无法可靠检查游戏进程",
                L"会话已失败关闭，没有对任何游戏进程执行写入。",
                PresentationTone::Error, true, true};
    case SessionResult::InstallationNotFoundNoWrite:
        return {L"没有找到 Steam 版文明 VI",
                L"请确认 Steam 安装和 appmanifest_289070.acf 可读；本次没有启动或写入。",
                PresentationTone::Error, true, true};
    case SessionResult::UnsupportedBuildNoWrite:
        return {L"当前构建不受支持",
                L"游戏哈希没有匹配的 Build Profile；工具按安全策略拒绝猜测偏移，未写入进程。",
                PresentationTone::Error, true, false};
    case SessionResult::HashFailedNoWrite:
        return {L"无法校验游戏文件",
                L"未能建立可靠的 SHA-256 与文件身份，因此没有启动或写入。",
                PresentationTone::Error, true, true};
    case SessionResult::SteamLaunchFailedNoWrite:
        return {L"Steam 启动请求失败",
                L"监听已建立，但 Steam 没有接受启动请求；没有写入任何游戏进程。",
                PresentationTone::Error, true, true};
    case SessionResult::TargetWaitTimedOutNoWrite:
        return {L"等待游戏启动超时",
                L"在限定时间内未发现 DX11 或 DX12 目标；没有写入任何进程。",
                PresentationTone::Warning, true, true};
    case SessionResult::RuntimeMismatchNoWrite:
        return {L"运行时安全校验未通过",
                L"远程 PE、文件身份、IAT 精确导出或 mutex 布局不匹配；未安装 Hook Guard。",
                PresentationTone::Error, true, false};
    case SessionResult::GuardInstallFailed:
        return {L"保护安装失败",
                L"没有发布 Hook Guard；详细的写入、页面保护、回读和所有权结果已写入日志。",
                PresentationTone::Error, true, true};
    case SessionResult::GuardInstallFailedNoHookProtectionUncertain:
        return {L"保护未发布，但页面保护状态异常",
                L"回读确认没有发布 Hook Guard，但 IAT 页面的原始保护未能恢复。请结束本局游戏后再重试。",
                PresentationTone::Error, true, false};
    case SessionResult::GuardInstallFailedRestoredAllocationRetained:
        return {L"保护发布失败，IAT 已恢复",
                L"最终回读确认 IAT 已恢复为原始目标。由于 Guard 可能短暂生效，远程内存不会热释放，将由游戏退出时回收。",
                PresentationTone::Error, true, false};
    case SessionResult::
        GuardInstallFailedRestoredProtectionUncertainAllocationRetained:
        return {L"IAT 已恢复，但页面保护状态无法确认",
                L"最终回读确认 IAT 已恢复为原始目标，但无法证明页面保护已恢复。远程内存保留到游戏退出；请结束本局游戏后再重试。",
                PresentationTone::Error, true, false};
    case SessionResult::GuardInstallFailedStateUncertainAllocationRetained:
        return {L"保护发布状态无法确认",
                L"无法证明 IAT 已恢复，因此没有继续覆盖它；远程内存会保留到游戏退出。请不要在本局游戏中重试，并查看日志。",
                PresentationTone::Error, true, false};
    case SessionResult::TargetWaitFailedHookKept:
        return {L"无法确认游戏进程状态",
                L"为避免错误恢复或释放，Hook Guard 保留到游戏实际退出；请查看日志。",
                PresentationTone::Error, true, false};
    }
    return {L"未知结果", L"请查看结构化日志。", PresentationTone::Error,
            true, true};
}

SessionPresentation PresentSessionStatus(const SessionStatus& status) {
    if (status.result != SessionResult::None) {
        SessionPresentation result = PresentSessionResult(status.result);
        if (!status.detail.empty()) {
            result.detail += L"\n\n" + status.detail;
        }
        return result;
    }
    switch (status.phase) {
    case SessionPhase::Created:
        return {L"准备就绪", L"即将开始安全检查。",
                PresentationTone::Neutral, false, false};
    case SessionPhase::Preparing:
        return {L"正在发现并校验游戏",
                L"读取 Steam 安装清单、检查已有进程并匹配 DX11/DX12 Build Profile。",
                PresentationTone::InProgress, false, false};
    case SessionPhase::Listening:
        return {L"监听已启动",
                L"推荐在 Steam 中手动启动并选择 DX11 或 DX12；也可以让工具请求 Steam 默认启动项，但该方式不保证渲染器。",
                PresentationTone::InProgress, false, false};
    case SessionPhase::WaitingForGame:
        return {L"正在等待文明 VI",
                L"请在 Steam 中手动启动并选择 DX11 或 DX12；工具会识别实际进程。自动请求 Steam 默认项不保证渲染器。",
                PresentationTone::InProgress, false, false};
    case SessionPhase::ValidatingRuntime:
        return {L"正在进行只读运行时校验",
                L"核对进程路径、文件身份、远程 PE 与精确 _Mtx_unlock 导出。",
                PresentationTone::InProgress, false, false};
    case SessionPhase::InstallingGuard:
        return {L"正在安装最小 Hook Guard",
                L"仅精确 Verified Profile 可进入此阶段；写入前再次比较 IAT 所有权。",
                PresentationTone::InProgress, false, false};
    case SessionPhase::Monitoring:
        return {L"保护已安装，正在确认 2K 状态",
                L"窗口无需确认；会话会自动给出结果。如需提前结束，请点击“停止并撤销保护”。",
                PresentationTone::InProgress, false, false};
    case SessionPhase::Completed:
        return PresentSessionResult(status.result);
    }
    return {L"正在工作", L"请稍候。", PresentationTone::InProgress, false,
            false};
}

SessionControls PresentSessionControls(const SessionStatus& status) {
    if (status.result != SessionResult::None ||
        status.phase == SessionPhase::Completed) {
        return {L"关闭窗口", SessionPrimaryAction::CloseWindow, true};
    }
    if (status.phase == SessionPhase::Monitoring) {
        return {L"停止并撤销保护", SessionPrimaryAction::RequestStop, true};
    }
    return {L"取消本次", SessionPrimaryAction::RequestStop, true};
}

SessionLaunchControl PresentSessionLaunchControl(
    const SessionStatus& status, bool steamLaunchRequested) {
    if (status.result != SessionResult::None ||
        status.phase == SessionPhase::Completed) {
        const SessionPresentation presentation = PresentSessionStatus(status);
        if (presentation.retryRecommended) {
            return {L"重新检测", SessionLaunchAction::RetrySession, true, true};
        }
        return {};
    }
    if (status.phase == SessionPhase::Created ||
        status.phase == SessionPhase::Preparing) {
        return {L"监听建立后可启动", SessionLaunchAction::None, false, true};
    }
    if (status.phase == SessionPhase::Listening ||
        status.phase == SessionPhase::WaitingForGame) {
        if (steamLaunchRequested) {
            return {L"已请求 Steam 默认项", SessionLaunchAction::None, false,
                    true};
        }
        return {L"让 Steam 启动（默认项）",
                SessionLaunchAction::RequestSteamDefault, true, true};
    }
    return {};
}

std::wstring BuildShareableDiagnostic(
    std::wstring_view productVersion, const SessionStatus& status,
    bool steamLaunchRequested) {
    SessionStatus shareableStatus = status;
    shareableStatus.detail.clear();
    const SessionPresentation presentation =
        PresentSessionStatus(shareableStatus);

    std::wostringstream text;
    text << productVersion << L"\r\n"
         << L"状态：" << presentation.headline << L"\r\n"
         << L"说明：" << presentation.detail << L"\r\n"
         << L"启动模式："
         << (steamLaunchRequested ? L"已请求 Steam 默认项"
                                  : L"仅监听（推荐）")
         << L"\r\n";
    if (status.profile != nullptr) {
        text << L"渲染器：" << RendererName(status.profile->renderer) << L"\r\n"
             << L"Profile：" << SupportStateName(status.profile->supportState)
             << L"\r\n"
             << L"拦截：" << status.skippedInvalidUnlocks << L"\r\n"
             << L"Discovery：" << status.discoveryState << L"\r\n"
             << L"SSO：" << status.ssoState << L"\r\n";
    } else {
        text << L"渲染器：尚未识别\r\n"
             << L"Profile：尚未识别\r\n";
    }
    text << L"隐私：此共享摘要已省略日志路径、PID 和内存地址；"
            L"不要直接公开原始 JSONL。";
    return text.str();
}

}  // namespace civ6fix
