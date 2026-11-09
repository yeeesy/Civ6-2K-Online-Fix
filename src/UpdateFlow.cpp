#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "UpdateFlow.h"
#include <bcrypt.h>
#include <shlobj.h>
#include <fstream>
#include <string_view>
#include <vector>

namespace civ6fix {
namespace fs=std::filesystem;
namespace {
bool MakePrivateDirectory(const fs::path& p) {
    if(!CreateDirectoryW(p.c_str(),nullptr) && GetLastError()!=ERROR_ALREADY_EXISTS)return false;
    auto a=GetFileAttributesW(p.c_str());
    return a!=INVALID_FILE_ATTRIBUTES && (a&FILE_ATTRIBUTE_DIRECTORY) && !(a&FILE_ATTRIBUTE_REPARSE_POINT);
}
bool WriteHelper(const fs::path& p) {
    auto r=FindResourceW(nullptr,MAKEINTRESOURCEW(101),MAKEINTRESOURCEW(10));
    if(!r)return false;
    const DWORD bytes=SizeofResource(nullptr,r);
    if(!bytes || bytes>1024*1024)return false;
    auto loaded=LoadResource(nullptr,r);
    if(!loaded)return false;
    void* data=LockResource(loaded);
    if(!data)return false;
    HANDLE f=CreateFileW(p.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(f==INVALID_HANDLE_VALUE)return false;
    DWORD written=0;
    bool ok=WriteFile(f,data,bytes,&written,nullptr) && written==bytes && FlushFileBuffers(f);
    CloseHandle(f);
    return ok;
}
}
bool CompleteUpdateHandoff() {
    // Only the authenticated update helper gives its newly launched child this token.
    // A failed handoff exits BEFORE a repair session starts.
    wchar_t token[33]{};
    DWORD n=GetEnvironmentVariableW(L"CIV6FIX_UPDATE_HANDOFF",token,33);
    if(!n)return GetLastError()==ERROR_ENVVAR_NOT_FOUND;
    if(n!=32 || std::wstring_view(token).find_first_not_of(L"0123456789abcdef")!=std::wstring_view::npos)return false;
    SetEnvironmentVariableW(L"CIV6FIX_UPDATE_HANDOFF",nullptr);
    auto ready=OpenEventW(EVENT_MODIFY_STATE,FALSE,(L"Local\\Civ6Fix-ready-"+std::wstring(token)).c_str());
    auto commit=OpenEventW(SYNCHRONIZE,FALSE,(L"Local\\Civ6Fix-commit-"+std::wstring(token)).c_str());
    bool ok=ready && commit && SetEvent(ready) && WaitForSingleObject(commit,20000)==WAIT_OBJECT_0;
    if(ready)CloseHandle(ready);
    if(commit)CloseHandle(commit);
    return ok;
}
UpdateFlow::~UpdateFlow(){Clear();}
void UpdateFlow::Clear() {
    if(process_){
        // This handle is exclusively our embedded network helper, never the game.
        if(!Applying() && WaitForSingleObject(process_,0)==WAIT_TIMEOUT) {
            TerminateProcess(process_,1);WaitForSingleObject(process_,1500);
        }
        CloseHandle(process_);process_=nullptr;
    }
    if(!dir_.empty() && !Applying()){
        std::error_code ec;
        for(auto n:{L"UpdateAgent.exe",L"bundle.zip",L"payload.exe",L"envelope.txt",L"version.txt",L"armed"})
            fs::remove(dir_/n,ec);
        fs::remove(dir_,ec); // Only removes an empty directory.
    }
    dir_.clear();phase_=Phase::Idle;version_.clear();
}
bool UpdateFlow::Spawn(const std::wstring& arguments,Phase phase) {
    if(dir_.empty() || target_.empty() || process_)return false;
    const auto helper=dir_/L"UpdateAgent.exe";
    std::wstring command=L"\""+helper.wstring()+L"\" "+arguments;
    STARTUPINFOW si{};si.cb=sizeof(si);
    PROCESS_INFORMATION pi{};
    if(!CreateProcessW(helper.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,dir_.c_str(),&si,&pi))return false;
    CloseHandle(pi.hThread);process_=pi.hProcess;phase_=phase;return true;
}
bool UpdateFlow::Check(bool manual) {
    if(Busy() || phase_==Phase::Ready)return false;
    Clear();
    wchar_t base[MAX_PATH]{};
    if(FAILED(SHGetFolderPathW(nullptr,CSIDL_LOCAL_APPDATA,nullptr,SHGFP_TYPE_CURRENT,base)))return false;
    auto root=fs::path(base)/L"Civ6_2K_Online_Fix";
    if(!MakePrivateDirectory(root) || !MakePrivateDirectory(root/L"Updates"))return false;
    UCHAR random[16]{};
    if(BCryptGenRandom(nullptr,random,sizeof(random),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)return false;
    std::wstring name=L"job-";
    for(auto b:random){name+=L"0123456789abcdef"[b>>4];name+=L"0123456789abcdef"[b&15];}
    auto path=root/L"Updates"/name;
    if(!CreateDirectoryW(path.c_str(),nullptr))return false;
    dir_=path;
    std::vector<wchar_t> module(32768);
    auto n=GetModuleFileNameW(nullptr,module.data(),static_cast<DWORD>(module.size()));
    if(!n || n>=module.size())return false;
    target_=std::wstring(module.data(),n);
    if(!WriteHelper(dir_/L"UpdateAgent.exe"))return false;
    return Spawn(L"check \""+target_.wstring()+L"\" "+(manual?L"manual":L"automatic"),Phase::Checking);
}
bool UpdateFlow::Download() {
    if(version_.empty())return false;
    return Spawn(L"prepare \""+target_.wstring()+L"\" "+version_,Phase::Downloading);
}
bool UpdateFlow::Apply() {
    if(phase_!=Phase::Ready)return false;
    FILETIME created{},exit{},kernel{},user{};
    if(!GetProcessTimes(GetCurrentProcess(),&created,&exit,&kernel,&user)){Clear();return false;}
    const auto time=(static_cast<unsigned long long>(created.dwHighDateTime)<<32)|created.dwLowDateTime;
    if(Spawn(L"apply \""+target_.wstring()+L"\" "+version_+L" "+std::to_wstring(GetCurrentProcessId())+L" "+std::to_wstring(time),Phase::Applying))return true;
    Clear();return false;
}
UpdateEvent UpdateFlow::Poll(){
    if(!process_)return UpdateEvent::None;
    const auto wait=WaitForSingleObject(process_,0);
    if(wait==WAIT_TIMEOUT){
        if(Applying() && GetFileAttributesW((dir_/L"armed").c_str())!=INVALID_FILE_ATTRIBUTES) return UpdateEvent::Armed;
        return UpdateEvent::None;
    }
    DWORD code=1;
    if(wait==WAIT_OBJECT_0)GetExitCodeProcess(process_,&code);
    CloseHandle(process_);process_=nullptr;
    auto before=phase_;phase_=Phase::Idle;
    if(before==Phase::Checking && code==20){
        std::ifstream f(dir_/L"version.txt",std::ios::binary);
        char buf[33]{}; f.read(buf,32);
        if(f.gcount()==0 || f.gcount()>=32)return UpdateEvent::Failed;
        std::string v(buf,static_cast<size_t>(f.gcount()));
        if(v.find_first_not_of("0123456789.")!=v.npos)return UpdateEvent::Failed;
        version_.assign(v.begin(),v.end());return UpdateEvent::Available;
    }
    if(before==Phase::Checking && code==0){Clear();return UpdateEvent::Current;}
    if(before==Phase::Downloading && code==0){phase_=Phase::Ready;return UpdateEvent::Downloaded;}
    Clear();return UpdateEvent::Failed;
}
}
