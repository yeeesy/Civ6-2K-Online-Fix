#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include "FeedbackFile.h"
#include "Logger.h"
#include "SessionPresentation.h"

namespace civ6fix {
namespace fs = std::filesystem;
namespace {
constexpr wchar_t prefix[] = L"问题反馈_文明六2K修复_";
bool PlainDirectory(const fs::path& dir) {
    DWORD a = GetFileAttributesW(dir.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) && !(a & FILE_ATTRIBUTE_REPARSE_POINT);
}
}
fs::path FeedbackDirectory() {
    wchar_t base[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, base))) return {};
    return fs::path(base) / L"Civ6_2K_Online_Fix" / L"Feedback";
}
fs::path LastFeedbackFile(const fs::path& dir) {
    if (dir.empty() || !PlainDirectory(dir) || !PlainDirectory(dir.parent_path())) return {};
    std::error_code ec;
    fs::path last;
    auto latest = fs::file_time_type::min();
    for (fs::directory_iterator it(dir,ec), end; !ec && it!=end; it.increment(ec)) {
        auto p=it->path();
        if (p.extension()!=L".txt" || p.filename().wstring().find(prefix)!=0) continue;
        DWORD a=GetFileAttributesW(p.c_str());
        if (a==INVALID_FILE_ATTRIBUTES || (a & (FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))) continue;
        const auto at=it->last_write_time(ec);
        if (!ec && (at>latest || (at==latest && p>last))) {latest=at; last=p;}
    }
    return last;
}
fs::path SaveFeedbackFile(const fs::path& dir, std::wstring_view version,
                          const SessionStatus& status, bool steamRequested) {
    if (dir.empty() || status.phase!=SessionPhase::Completed || version.empty() || version.size()>40 ||
        version.find_first_not_of(L"0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ.-")!=version.npos) return {};
    std::error_code ec;
    fs::create_directories(dir.parent_path(),ec);
    if (ec || !PlainDirectory(dir.parent_path())) return {};
    fs::create_directory(dir,ec);
    if (ec || !PlainDirectory(dir)) return {};
    std::wstring title=PresentPlayerStatus(status).headline;
    for (auto& ch:title) if (ch<32 || std::wstring_view(L"\\/:*?\"<>|").find(ch)!=std::wstring_view::npos) ch=L'_';
    auto text=BuildShareableDiagnostic(L"Civ6 2K Online Fix "+std::wstring(version),status,steamRequested);
    text += L"\r\n\r\n可选补充：游戏内 2K 在线 / 离线 / 未确认：\r\n是否已到主菜单、能否联机：\r\n";
    const auto bytes=std::string("\xEF\xBB\xBF")+Utf8(text);
    if (bytes.size()>16*1024*1024 || bytes.size()<=3) return {};
    const auto stem=std::wstring(prefix)+std::wstring(version)+L"_"+title+L"_";
    for (unsigned i=1; i<=99999; ++i) {
        const auto path=dir/(stem+std::to_wstring(i)+L".txt");
        auto file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if (file==INVALID_HANDLE_VALUE) {
            if (GetLastError()==ERROR_FILE_EXISTS) continue;
            return {};
        }
        DWORD written=0;
        bool ok=WriteFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr) &&
                written==bytes.size() && FlushFileBuffers(file);
        CloseHandle(file);
        if (!ok) {DeleteFileW(path.c_str());return {};}
        return path;
    }
    return {};
}
} // namespace civ6fix
