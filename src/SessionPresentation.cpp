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

const wchar_t* PhaseCode(int phase) noexcept {
    switch (static_cast<SessionPhase>(phase)) {
    case SessionPhase::Created: return L"created";
    case SessionPhase::Preparing: return L"preparing";
    case SessionPhase::Listening: return L"listening";
    case SessionPhase::WaitingForGame: return L"waiting-game";
    case SessionPhase::ValidatingRuntime: return L"validating-runtime";
    case SessionPhase::InstallingGuard: return L"installing-guard";
    case SessionPhase::Monitoring: return L"monitoring";
    case SessionPhase::Completed: return L"completed";
    }
    return L"unknown";
}

const wchar_t* ResultCode(SessionResult result) noexcept {
    // Fixed allowlist; never forward a Win32 error message or platform detail.
#define RESULT_CODE(name) case SessionResult::name: return L## #name
    switch (result) {
    RESULT_CODE(None); RESULT_CODE(Fixed); RESULT_CODE(OnlineWithoutIntervention);
    RESULT_CODE(MonitorTimedOutHookKept); RESULT_CODE(CandidateDetectedReadOnly);
    RESULT_CODE(TargetExited); RESULT_CODE(CancelledBeforeWrite);
    RESULT_CODE(CancelledOwnedHookRestoredAllocationRetained);
    RESULT_CODE(CancelledOwnedHookRestoredProtectionUncertainAllocationRetained);
    RESULT_CODE(CancelledHookNotOwnedNoWrite); RESULT_CODE(ExistingGameNoWrite);
    RESULT_CODE(ProcessScanFailedNoWrite); RESULT_CODE(InstallationNotFoundNoWrite);
    RESULT_CODE(UnsupportedBuildNoWrite); RESULT_CODE(HashFailedNoWrite);
    RESULT_CODE(SteamLaunchFailedNoWrite); RESULT_CODE(TargetWaitTimedOutNoWrite);
    RESULT_CODE(RuntimeMismatchNoWrite); RESULT_CODE(GuardInstallFailed);
    RESULT_CODE(GuardInstallFailedNoHookProtectionUncertain);
    RESULT_CODE(GuardInstallFailedRestoredAllocationRetained);
    RESULT_CODE(GuardInstallFailedRestoredProtectionUncertainAllocationRetained);
    RESULT_CODE(GuardInstallFailedStateUncertainAllocationRetained);
    RESULT_CODE(TargetWaitFailedHookKept);
    }
#undef RESULT_CODE
    return L"Unknown";
}

const wchar_t* QualityName(ReadQuality quality) noexcept {
    switch (quality) {
    case ReadQuality::NotSampled: return L"未采样";
    case ReadQuality::Value: return L"可读";
    case ReadQuality::NotCreated: return L"对象尚未创建";
    case ReadQuality::PointerReadFailed: return L"对象指针读取失败";
    case ReadQuality::ValueReadFailed: return L"数值读取失败";
    case ReadQuality::InvalidAddress: return L"地址范围不合法";
    case ReadQuality::Unavailable: return L"读取失败/上下文不可用";
    }
    return L"未知读取状态";
}

const wchar_t* ValidationName(int value) noexcept {
    switch (static_cast<RuntimeValidationOutcome>(value)) {
    case RuntimeValidationOutcome::Valid: return L"valid";
    case RuntimeValidationOutcome::ProcessExited: return L"process-exited";
    case RuntimeValidationOutcome::ProcessIdentityMismatch: return L"process-identity-mismatch";
    case RuntimeValidationOutcome::ImagePathMismatch: return L"image-path-mismatch";
    case RuntimeValidationOutcome::PeIdentityMismatch: return L"pe-identity-mismatch";
    case RuntimeValidationOutcome::IatTargetMismatch: return L"iat-target-mismatch";
    case RuntimeValidationOutcome::MutexLayoutMismatch: return L"mutex-layout-mismatch";
    case RuntimeValidationOutcome::ReadFailed: return L"read-failed";
    }
    return L"unknown";
}

const wchar_t* InstallName(int value) noexcept {
    switch (static_cast<GuardInstallOutcome>(value)) {
    case GuardInstallOutcome::Installed: return L"installed";
    case GuardInstallOutcome::FailedNoHook: return L"failed-no-hook";
    case GuardInstallOutcome::FailedNoHookProtectionUncertain: return L"failed-no-hook-protection-uncertain";
    case GuardInstallOutcome::FailedRestoredAllocationRetained: return L"failed-restored-retained";
    case GuardInstallOutcome::FailedRestoredProtectionUncertainAllocationRetained: return L"failed-restored-protection-uncertain-retained";
    case GuardInstallOutcome::FailedStateUncertainAllocationRetained: return L"failed-state-uncertain-retained";
    }
    return L"unknown";
}

const wchar_t* TimeoutCategory(const SessionDiagnostics& d) noexcept {
    if (d.samples == 0) return L"T0-NO-SAMPLE：没有取得监控样本";
    if (d.latest.counter.quality != ReadQuality::Value)
        return L"T1-COUNTER-UNREADABLE：末次拦截计数不可读";
    const auto badRead = [](ReadQuality q) {
        return q != ReadQuality::Value && q != ReadQuality::NotCreated;
    };
    if (badRead(d.latest.discovery.quality) || badRead(d.latest.sso.quality))
        return L"T2-STATE-UNREADABLE：末次服务状态读取不完整";
    if (d.latest.discovery.quality == ReadQuality::NotCreated || d.latest.sso.quality == ReadQuality::NotCreated)
        return L"T3-OBJECT-NOT-CREATED：末次服务对象尚未创建";
    if (d.latest.skippedInvalidUnlocks != 0)
        return L"T4-INTERCEPTED-NOT-READY：已拦截，但未确认同时就绪";
    return L"T5-NO-INTERCEPTION-NOT-READY：未观察到拦截，未确认同时就绪";
}

template <typename Number>
void WriteField(std::wostringstream& text, const wchar_t* name,
                const FieldRead& field, Number value, const FieldStatistics& stats,
                Number lastGood, std::uint64_t elapsed) {
    text << name << L"：";
    if (field.quality == ReadQuality::Value) text << value << L"；";
    text << QualityName(field.quality) << L"；Win32=" << field.error
         << L"；成功/对象未创建/读取失败=" << stats.values << L"/" << stats.absent << L"/" << stats.failures;
    if (stats.values != 0) {
        text << L"；最近有效=" << lastGood << L"（距快照 "
             << (elapsed >= stats.lastValueAtMs ? elapsed - stats.lastValueAtMs : 0) << L" 毫秒）";
    } else {
        text << L"；从未取得有效值";
    }
    text << L"；最近非零错误=" << stats.lastError << L"\r\n";
}

template <typename Number>
void WriteEventField(std::wostringstream& text, const FieldRead& field, Number value) {
    if (field.quality == ReadQuality::Value) text << value;
    else text << QualityName(field.quality) << L"(Win32=" << field.error << L")";
}

void WriteEvidence(std::wostringstream& text, const SessionStatus& status) {
    const auto& d = status.diagnostics;
    text << L"诊断格式：2（增强脱敏诊断）\r\n"
         << L"结果码：" << ResultCode(status.result) << L"\r\n"
         << L"当前阶段：" << PhaseCode(static_cast<int>(status.phase)) << L"\r\n";
    if (!d.available) {
        text << L"采集：尚未开始；以下旧式数值无读取质量和时间证据\r\n";
        return;
    }
    text << L"最后活动阶段：" << PhaseCode(d.lastActivePhase) << L"\r\n"
         << L"会话耗时：" << d.elapsedMs << L" 毫秒\r\n阶段耗时（毫秒）：";
    for (std::size_t i = 0; i < d.phaseMs.size() - 1; ++i)
        text << PhaseCode(static_cast<int>(i)) << L"=" << d.phaseMs[i] << L" ";
    text << L"\r\n等待游戏上限/监控上限/轮询间隔（毫秒）："
         << d.waitTimeoutMs << L"/" << d.monitorTimeoutMs << L"/" << d.monitorPollMs
         << L"\r\n进程扫描 Win32：" << d.scanError
         << L"\r\n运行时校验：" << (d.validationAttempted ? ValidationName(d.validationOutcome) : L"未执行")
         << L"；Win32=" << d.validationError
         << L"\r\nGuard 安装：" << (d.installAttempted ? InstallName(d.installOutcome) : L"未执行")
         << L"；Win32=" << d.installError << L"\r\n";
    if (d.guardInstalled) {
        text << L"安装时游戏进程年龄：";
        if (d.processAgeAtGuardMs) text << *d.processAgeAtGuardMs << L" 毫秒（系统时钟估算，不能证明介入足够早）";
        else text << L"未知";
        text << L"\r\n";
    }
    text << L"安装前锁探测：";
    switch (d.mutexProbe) {
    case VectorProbeState::NotAttempted: text << L"未探测"; break;
    case VectorProbeState::ReadFailed: text << L"读取失败"; break;
    case VectorProbeState::NotCreated: text << L"向量为空（尚未创建）"; break;
    case VectorProbeState::Available: text << L"布局可读"; break;
    case VectorProbeState::InvalidLayout: text << L"布局不符"; break;
    }
    if (d.mutexCountBeforeGuard) text << L"；锁计数=" << *d.mutexCountBeforeGuard;
    text << L"\r\n监控耗时：" << d.monitorElapsedMs << L" 毫秒；样本/完整样本="
         << d.samples << L"/" << d.completeSamples << L"；最大采样间隔=" << d.maxSampleGapMs << L" 毫秒\r\n";
    if (d.samples) text << L"末次采样距快照：" << d.monitorElapsedMs - d.lastSampleAtMs << L" 毫秒\r\n";
    WriteField(text, L"拦截", d.latest.counter, d.latest.skippedInvalidUnlocks, d.counter, d.lastGoodCounter, d.monitorElapsedMs);
    WriteField(text, L"Discovery", d.latest.discovery, d.latest.discoveryState, d.discovery, d.lastGoodDiscovery, d.monitorElapsedMs);
    WriteField(text, L"SSO", d.latest.sso, d.latest.ssoState, d.sso, d.lastGoodSso, d.monitorElapsedMs);
    text << L"首次观察到拦截：";
    if (d.firstInterceptionAtMs) text << L"监控开始后 " << *d.firstInterceptionAtMs << L" 毫秒";
    else text << L"未观察到（不等于此前从未发生）";
    text << L"\r\n";
    if (status.result == SessionResult::MonitorTimedOutHookKept)
        text << L"超时分类：" << TimeoutCategory(d) << L"\r\n";
    text << L"状态变化时间线（相对监控开始；毫秒；C=拦截/D=Discovery/S=SSO）：\r\n";
    for (std::size_t i = 0; i < d.eventCount && i < d.events.size(); ++i) {
        if (i == SessionDiagnostics::kKeepFirst && d.omittedEvents)
            text << L"  …省略中间 " << d.omittedEvents << L" 次变化…\r\n";
        const auto& event = d.events[i];
        text << L"  +" << event.atMs << L" C=";
        WriteEventField(text, event.sample.counter, event.sample.skippedInvalidUnlocks);
        text << L" D="; WriteEventField(text, event.sample.discovery, event.sample.discoveryState);
        text << L" S="; WriteEventField(text, event.sample.sso, event.sample.ssoState);
        text << L"\r\n";
    }
    text << L"说明：仅状态 4 按已验证就绪处理；其他数值原样记录，未建立阶段含义映射。三个字段为顺序读取，非原子快照。分类是排查入口，不是根因判定。\r\n"
         << L"SQLite/网络/账号状态：本工具未检查；不会据此判断数据库损坏或排除网络问题。\r\n"
         << L"反馈请补充：游戏内实际在线/离线；是否到主菜单；每次还是偶发；是否做过缓存操作（只给文件名和脱敏相对位置）；如已有前后报告请同时提供。不要为采集报告删除数据库。\r\n";
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
                L"限定时间内未确认 2K 同时就绪，不等于已判定修复失败或数据库损坏。高频监控已停止，Hook Guard 保留到游戏退出；现在可关闭窗口。请复制脱敏诊断反馈，并补充游戏内实际状态；需要重试时请先自行退出游戏。",
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
             << L"\r\n";
        if (!status.diagnostics.available) {
            text << L"拦截：" << status.skippedInvalidUnlocks << L"\r\n"
                 << L"Discovery：" << status.discoveryState << L"\r\n"
                 << L"SSO：" << status.ssoState << L"\r\n";
        }
    } else {
        text << L"渲染器：尚未识别\r\n"
             << L"Profile：尚未识别\r\n";
    }
    WriteEvidence(text, status);
    text << L"隐私：此共享摘要已省略日志路径、PID 和内存地址；不含用户名、账号、数据库内容或原始错误文本；"
            L"不要直接公开原始 JSONL。";
    return text.str();
}

}  // namespace civ6fix
