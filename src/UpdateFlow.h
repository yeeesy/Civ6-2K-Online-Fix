#pragma once
#include <windows.h>
#include <filesystem>
#include <string>

namespace civ6fix {
bool CompleteUpdateHandoff();
enum class UpdateEvent { None, Current, Available, Downloaded, Armed, Failed };
class UpdateFlow {
public:
    ~UpdateFlow();
    bool Check(bool manual);
    bool Download();
    bool Apply();
    UpdateEvent Poll();
    bool Busy() const noexcept {return process_ != nullptr;}
    bool Applying() const noexcept {return phase_ == Phase::Applying;}
    const std::wstring& Version() const noexcept {return version_;}
private:
    enum class Phase { Idle, Checking, Downloading, Ready, Applying };
    bool Spawn(const std::wstring& arguments,Phase phase);
    void Clear();
    HANDLE process_ = nullptr;
    Phase phase_ = Phase::Idle;
    std::filesystem::path dir_,target_;
    std::wstring version_;
};
}
