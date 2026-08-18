#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <psapi.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include "Win32FixPlatform.h"

#include "Logger.h"
#include "SteamDiscovery.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace civ6fix {
namespace {

constexpr wchar_t kSteamAppId[] = L"289070";
constexpr wchar_t kDx11Executable[] = L"CivilizationVI.exe";
constexpr wchar_t kDx12Executable[] = L"CivilizationVI_DX12.exe";
constexpr wchar_t kGameBinaryRelative[] =
    L"Base\\Binaries\\Win64Steam";
constexpr std::uint32_t kRuntimeWaitMs = 30U * 1000U;

const char* SupportStateLogName(SupportState state) noexcept {
    switch (state) {
    case SupportState::Verified: return "verified";
    case SupportState::Experimental: return "experimental";
    case SupportState::ReadOnlyCandidate: return "read_only_candidate";
    case SupportState::Unsupported: return "unsupported";
    }
    return "unsupported";
}

bool BuildUseAllowsGuard(BuildUseDecision decision) noexcept {
    return decision == BuildUseDecision::InstallVerifiedGuard;
}

class ScopedHandle {
public:
    ScopedHandle() = default;
    explicit ScopedHandle(HANDLE handle) : handle_(handle) {}
    ~ScopedHandle() { Reset(); }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ScopedHandle(ScopedHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    ScopedHandle& operator=(ScopedHandle&& other) noexcept {
        if (this != &other) {
            Reset();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    bool Valid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

    HANDLE Get() const noexcept { return handle_; }

    void Reset(HANDLE next = nullptr) noexcept {
        if (Valid()) {
            CloseHandle(handle_);
        }
        handle_ = next;
    }

private:
    HANDLE handle_ = nullptr;
};

bool EqualsPathInsensitive(const std::filesystem::path& left,
                           const std::filesystem::path& right) {
    return EqualsInsensitive(left.lexically_normal().wstring(),
                             right.lexically_normal().wstring());
}

bool IsCivExecutableName(const wchar_t* value) {
    return _wcsicmp(value, kDx11Executable) == 0 ||
           _wcsicmp(value, kDx12Executable) == 0;
}

std::optional<std::string> ReadTextFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }
    file.seekg(0, std::ios::end);
    const std::streamoff length = file.tellg();
    if (length < 0 ||
        static_cast<std::uint64_t>(length) > kMaxSteamMetadataBytes) {
        return std::nullopt;
    }
    file.seekg(0, std::ios::beg);
    std::string text(static_cast<std::size_t>(length), '\0');
    if (!text.empty()) {
        file.read(text.data(), static_cast<std::streamsize>(text.size()));
        if (file.gcount() != static_cast<std::streamsize>(text.size())) {
            return std::nullopt;
        }
    }
    char extra = 0;
    if (file.read(&extra, 1)) {
        return std::nullopt;
    }
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
    return text;
}

std::optional<std::wstring> ReadRegistryString(HKEY root, const wchar_t* subkey,
                                                const wchar_t* value,
                                                REGSAM view = 0) {
    HKEY key = nullptr;
#pragma warning(suppress : 6553)  // Windows SDK SAL annotation noise under /analyze.
    if (RegOpenKeyExW(root, subkey, 0, KEY_QUERY_VALUE | view, &key) !=
        ERROR_SUCCESS) {
        return std::nullopt;
    }
    DWORD type = 0;
    DWORD bytes = 0;
    LONG status = RegQueryValueExW(key, value, nullptr, &type, nullptr, &bytes);
    if (status != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t)) {
        RegCloseKey(key);
        return std::nullopt;
    }
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
    status = RegQueryValueExW(key, value, nullptr, &type,
                              reinterpret_cast<BYTE*>(buffer.data()), &bytes);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS) {
        return std::nullopt;
    }
    std::wstring result(buffer.data());
    if (type == REG_EXPAND_SZ) {
        const DWORD required = ExpandEnvironmentStringsW(result.c_str(), nullptr, 0);
        if (required > 1) {
            std::wstring expanded(required, L'\0');
            if (ExpandEnvironmentStringsW(result.c_str(), expanded.data(), required) ==
                required) {
                expanded.resize(required - 1);
                result = std::move(expanded);
            }
        }
    }
    return result.empty() ? std::nullopt
                          : std::optional<std::wstring>(std::move(result));
}

void AddUniqueRoot(std::vector<std::filesystem::path>& roots,
                   std::filesystem::path root) {
    if (root.empty()) {
        return;
    }
    root = root.lexically_normal();
    if (EqualsInsensitive(root.filename().wstring(), L"steam.exe")) {
        root = root.parent_path();
    }
    const auto duplicate = std::find_if(
        roots.begin(), roots.end(), [&](const std::filesystem::path& existing) {
            return EqualsPathInsensitive(existing, root);
        });
    if (duplicate == roots.end()) {
        roots.push_back(std::move(root));
    }
}

bool IsSupportedDrivePath(const std::filesystem::path& path) {
    const std::filesystem::path root = path.root_path();
    if (root.empty() || root.native().size() < 3) {
        return false;
    }
    const UINT driveType = GetDriveTypeW(root.c_str());
    return driveType != DRIVE_UNKNOWN && driveType != DRIVE_NO_ROOT_DIR &&
           driveType != DRIVE_REMOTE;
}

std::vector<std::filesystem::path> SteamRootCandidates() {
    std::vector<std::filesystem::path> roots;
    for (const auto& value : {
             ReadRegistryString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam",
                                L"SteamPath"),
             ReadRegistryString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam",
                                L"SteamExe"),
             ReadRegistryString(HKEY_LOCAL_MACHINE, L"Software\\Valve\\Steam",
                                L"InstallPath", KEY_WOW64_32KEY),
             ReadRegistryString(HKEY_LOCAL_MACHINE, L"Software\\Valve\\Steam",
                                L"InstallPath", KEY_WOW64_64KEY),
         }) {
        if (value.has_value()) {
            AddUniqueRoot(roots, std::filesystem::path(*value));
        }
    }

    std::vector<wchar_t> programFiles(32768);
    const DWORD length = GetEnvironmentVariableW(
        L"ProgramFiles(x86)", programFiles.data(),
        static_cast<DWORD>(programFiles.size()));
    if (length > 0 && length < programFiles.size()) {
        AddUniqueRoot(roots,
                      std::filesystem::path(programFiles.data()) / L"Steam");
    }
    return roots;
}

std::wstring Sha256Handle(HANDLE file) {
    LARGE_INTEGER beginning{};
    if (file == nullptr || file == INVALID_HANDLE_VALUE ||
        !SetFilePointerEx(file, beginning, nullptr, FILE_BEGIN)) {
        return {};
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hashHandle = nullptr;
    std::vector<UCHAR> object;
    std::vector<UCHAR> digest;
    std::wstring result;
    do {
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr,
                                        0) < 0) {
            break;
        }
        DWORD objectLength = 0;
        DWORD digestLength = 0;
        DWORD bytes = 0;
        if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                              reinterpret_cast<PUCHAR>(&objectLength),
                              sizeof(objectLength), &bytes, 0) < 0 ||
            BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                              reinterpret_cast<PUCHAR>(&digestLength),
                              sizeof(digestLength), &bytes, 0) < 0) {
            break;
        }
        object.resize(objectLength);
        digest.resize(digestLength);
        if (BCryptCreateHash(algorithm, &hashHandle, object.data(), objectLength,
                             nullptr, 0, 0) < 0) {
            break;
        }

        std::vector<UCHAR> buffer(64U * 1024U);
        bool readOk = true;
        for (;;) {
            DWORD read = 0;
            if (!ReadFile(file, buffer.data(),
                          static_cast<DWORD>(buffer.size()), &read, nullptr)) {
                readOk = false;
                break;
            }
            if (read == 0) {
                break;
            }
            if (BCryptHashData(hashHandle, buffer.data(), read, 0) < 0) {
                readOk = false;
                break;
            }
        }
        if (!readOk ||
            BCryptFinishHash(hashHandle, digest.data(), digestLength, 0) < 0) {
            break;
        }

        std::wostringstream stream;
        stream << std::uppercase << std::hex << std::setfill(L'0');
        for (UCHAR byte : digest) {
            stream << std::setw(2) << static_cast<unsigned int>(byte);
        }
        result = stream.str();
    } while (false);

    if (hashHandle != nullptr) {
        BCryptDestroyHash(hashHandle);
    }
    if (algorithm != nullptr) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    return result;
}

struct FileFingerprint {
    bool valid = false;
    std::uint64_t size = 0;
    std::uint64_t lastWrite = 0;
    std::uint64_t fileIndex = 0;
    DWORD volumeSerial = 0;
};

FileFingerprint FingerprintHandle(HANDLE file) {
    if (file == nullptr || file == INVALID_HANDLE_VALUE) {
        return {};
    }
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(file, &information)) {
        return {};
    }
    FileFingerprint result;
    result.valid = true;
    result.size = (static_cast<std::uint64_t>(information.nFileSizeHigh) << 32U) |
                  information.nFileSizeLow;
    result.lastWrite =
        (static_cast<std::uint64_t>(information.ftLastWriteTime.dwHighDateTime)
         << 32U) |
        information.ftLastWriteTime.dwLowDateTime;
    result.fileIndex =
        (static_cast<std::uint64_t>(information.nFileIndexHigh) << 32U) |
        information.nFileIndexLow;
    result.volumeSerial = information.dwVolumeSerialNumber;
    return result;
}

bool SameFingerprint(const FileFingerprint& left,
                      const FileFingerprint& right) {
    return left.valid && right.valid && left.size == right.size &&
           left.lastWrite == right.lastWrite && left.fileIndex == right.fileIndex &&
           left.volumeSerial == right.volumeSerial;
}

bool TryGetProcessCreationTime(HANDLE process, std::uint64_t& creationTime) {
    if (process == nullptr || process == INVALID_HANDLE_VALUE) {
        return false;
    }
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetProcessTimes(process, &creation, &exit, &kernel, &user)) {
        return false;
    }
    creationTime =
        (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32U) |
        creation.dwLowDateTime;
    return creationTime != 0;
}

bool SameProcessObject(HANDLE left, HANDLE right) {
    if (left == nullptr || right == nullptr || GetProcessId(left) != GetProcessId(right)) {
        return false;
    }
    std::uint64_t leftCreation = 0;
    std::uint64_t rightCreation = 0;
    return TryGetProcessCreationTime(left, leftCreation) &&
           TryGetProcessCreationTime(right, rightCreation) &&
           leftCreation == rightCreation;
}

template <typename T>
bool ReadRemote(HANDLE process, std::uintptr_t address, T& value) {
    SIZE_T read = 0;
    return ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), &value,
                             sizeof(value), &read) != FALSE &&
           read == sizeof(value);
}

bool WriteRemote(HANDLE process, std::uintptr_t address, const void* data,
                 std::size_t size) {
    SIZE_T written = 0;
    return WriteProcessMemory(process, reinterpret_cast<LPVOID>(address), data,
                              size, &written) != FALSE &&
           written == size;
}

bool IsExecutableProtection(DWORD protection) {
    protection &= 0xFFU;
    return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
           protection == PAGE_EXECUTE_READWRITE ||
           protection == PAGE_EXECUTE_WRITECOPY;
}

bool IsReadableProtection(DWORD protection) {
    if ((protection & PAGE_GUARD) != 0) {
        return false;
    }
    protection &= 0xFFU;
    return protection == PAGE_READONLY || protection == PAGE_READWRITE ||
           protection == PAGE_WRITECOPY || IsExecutableProtection(protection);
}

bool IsCommittedImageSpan(HANDLE process, std::uintptr_t address,
                          std::size_t span,
                          std::uintptr_t expectedAllocationBase,
                          bool requireExecutable) {
    if (span == 0) {
        return false;
    }
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQueryEx(process, reinterpret_cast<LPCVOID>(address), &memory,
                       sizeof(memory)) != sizeof(memory) ||
        memory.State != MEM_COMMIT || memory.Type != MEM_IMAGE ||
        memory.AllocationBase !=
            reinterpret_cast<PVOID>(expectedAllocationBase) ||
        (requireExecutable ? !IsExecutableProtection(memory.Protect)
                           : !IsReadableProtection(memory.Protect))) {
        return false;
    }
    const std::uintptr_t regionBase =
        reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
    return address >= regionBase && address - regionBase <= memory.RegionSize &&
           span <= memory.RegionSize - (address - regionBase);
}

struct RemoteModule {
    std::uintptr_t base = 0;
    std::uint32_t size = 0;
    std::wstring name;
    std::filesystem::path path;
};

bool IsExpectedSystemRuntimePath(const std::filesystem::path& path,
                                 std::wstring_view moduleName) {
    std::vector<wchar_t> systemDirectory(32768);
    const UINT length = GetSystemDirectoryW(
        systemDirectory.data(), static_cast<UINT>(systemDirectory.size()));
    if (length == 0 || length >= systemDirectory.size()) {
        return false;
    }
    const std::filesystem::path expected =
        std::filesystem::path(
            std::wstring(systemDirectory.data(), static_cast<std::size_t>(length))) /
        moduleName;
    return EqualsPathInsensitive(path, expected);
}

std::optional<RemoteModule> FindMainModule(DWORD pid,
                                           std::wstring_view executableName) {
    ScopedHandle snapshot(CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
    if (!snapshot.Valid()) {
        return std::nullopt;
    }
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Module32FirstW(snapshot.Get(), &entry)) {
        return std::nullopt;
    }
    do {
        if (EqualsInsensitive(entry.szModule, executableName)) {
            return RemoteModule{
                reinterpret_cast<std::uintptr_t>(entry.modBaseAddr),
                entry.modBaseSize,
                entry.szModule,
                entry.szExePath,
            };
        }
    } while (Module32NextW(snapshot.Get(), &entry));
    return std::nullopt;
}

std::optional<RemoteModule> FindModuleContaining(DWORD pid,
                                                 std::uintptr_t address) {
    ScopedHandle snapshot(CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
    if (!snapshot.Valid()) {
        return std::nullopt;
    }
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Module32FirstW(snapshot.Get(), &entry)) {
        return std::nullopt;
    }
    do {
        const std::uintptr_t base =
            reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
        if (address >= base && address - base < entry.modBaseSize) {
            return RemoteModule{base, entry.modBaseSize, entry.szModule,
                                entry.szExePath};
        }
    } while (Module32NextW(snapshot.Get(), &entry));
    return std::nullopt;
}

std::optional<std::uintptr_t> ResolveRemoteExport(
    const RemoteModule& module, const char* exportName) {
    HMODULE local = LoadLibraryExW(module.path.c_str(), nullptr,
                                   DONT_RESOLVE_DLL_REFERENCES);
    if (local == nullptr) {
        return std::nullopt;
    }
    FARPROC exported = GetProcAddress(local, exportName);
    if (exported == nullptr) {
        FreeLibrary(local);
        return std::nullopt;
    }
    const std::uintptr_t rva = reinterpret_cast<std::uintptr_t>(exported) -
                               reinterpret_cast<std::uintptr_t>(local);
    FreeLibrary(local);
    if (rva >= module.size || !IsAddressAdditionSafe(module.base, rva)) {
        return std::nullopt;
    }
    return module.base + rva;
}

struct PatchAttempt {
    PointerPatchEvidence evidence{};
    PointerPatchOutcome outcome = PointerPatchOutcome::MakeWritableFailed;
    std::uintptr_t observed = 0;
    DWORD originalProtection = 0;
    DWORD win32Error = ERROR_SUCCESS;
};

PatchAttempt PatchPointerChecked(HANDLE process, std::uintptr_t address,
                                 std::uintptr_t expectedCurrent,
                                 std::uintptr_t replacement,
                                 std::optional<DWORD> requestedFinalProtection =
                                     std::nullopt) {
    PatchAttempt attempt;
    DWORD oldProtection = 0;
    attempt.evidence.madeWritable =
        VirtualProtectEx(process, reinterpret_cast<LPVOID>(address),
                         sizeof(replacement), PAGE_READWRITE,
                         &oldProtection) != FALSE;
    if (!attempt.evidence.madeWritable) {
        attempt.win32Error = GetLastError();
        attempt.outcome = EvaluatePointerPatch(attempt.evidence);
        return attempt;
    }
    attempt.originalProtection = oldProtection;

    std::uintptr_t current = 0;
    const bool ownershipRead = ReadRemote(process, address, current);
    attempt.evidence.ownershipMatched =
        ownershipRead && current == expectedCurrent;
    if (attempt.evidence.ownershipMatched) {
        attempt.evidence.writeCompleted =
            WriteRemote(process, address, &replacement, sizeof(replacement));
        if (!attempt.evidence.writeCompleted) {
            attempt.win32Error = GetLastError();
        }
    } else {
        attempt.win32Error = ownershipRead ? ERROR_INVALID_STATE : GetLastError();
    }

    DWORD finalProtection = oldProtection;
    bool requestedProtectionCanBeProven = !requestedFinalProtection.has_value();
    if (requestedFinalProtection.has_value()) {
        if (attempt.evidence.ownershipMatched) {
            finalProtection = *requestedFinalProtection;
            requestedProtectionCanBeProven = true;
        } else if (oldProtection == *requestedFinalProtection) {
            requestedProtectionCanBeProven = true;
        }
    }
    DWORD ignored = 0;
    const bool protectionCallSucceeded =
        VirtualProtectEx(process, reinterpret_cast<LPVOID>(address),
                         sizeof(replacement), finalProtection, &ignored) != FALSE;
    const DWORD protectionError =
        protectionCallSucceeded ? ERROR_SUCCESS : GetLastError();
    attempt.evidence.protectionRestored =
        protectionCallSucceeded && requestedProtectionCanBeProven;
    if (!attempt.evidence.protectionRestored &&
        attempt.win32Error == ERROR_SUCCESS) {
        attempt.win32Error = protectionCallSucceeded ? ERROR_INVALID_STATE
                                                     : protectionError;
    }

    attempt.evidence.readBackCompleted =
        ReadRemote(process, address, attempt.observed);
    attempt.evidence.expectedValueObserved =
        attempt.evidence.readBackCompleted && attempt.observed == replacement;
    if (!attempt.evidence.readBackCompleted &&
        attempt.win32Error == ERROR_SUCCESS) {
        attempt.win32Error = GetLastError();
    }
    attempt.outcome = EvaluatePointerPatch(attempt.evidence);
    return attempt;
}

std::string PatchOutcomeName(PointerPatchOutcome outcome) {
    switch (outcome) {
    case PointerPatchOutcome::Succeeded: return "succeeded";
    case PointerPatchOutcome::MakeWritableFailed: return "make_writable_failed";
    case PointerPatchOutcome::WriteFailed: return "write_failed";
    case PointerPatchOutcome::ProtectionRestoreFailed:
        return "protection_restore_failed";
    case PointerPatchOutcome::OwnershipChanged: return "ownership_changed";
    case PointerPatchOutcome::ReadBackFailed: return "read_back_failed";
    case PointerPatchOutcome::VerificationFailed: return "verification_failed";
    }
    return "unknown";
}

}  // namespace

struct Win32FixPlatform::Impl {
    struct CachedBuild {
        std::filesystem::path path;
        std::wstring executableName;
        std::wstring sha256;
        const BuildProfile* profile = nullptr;
        FileFingerprint fingerprint;
        ScopedHandle pinnedFile;
    };

    explicit Impl(Logger& value) : logger(value) {}

    ~Impl() { process.Reset(); }

    Logger& logger;
    std::filesystem::path steamRoot;
    std::filesystem::path steamExecutable;
    std::filesystem::path gameRoot;
    std::vector<CachedBuild> builds;
    ScopedHandle process;
    DWORD targetPid = 0;
    std::uintptr_t moduleBase = 0;
    std::uintptr_t validatedIat = 0;
    std::uintptr_t validatedOriginal = 0;
    const BuildProfile* activeProfile = nullptr;

    const CachedBuild* FindBuild(const std::filesystem::path& path) const {
        const auto found = std::find_if(
            builds.begin(), builds.end(), [&](const CachedBuild& candidate) {
                return EqualsPathInsensitive(candidate.path, path);
            });
        return found == builds.end() ? nullptr : &*found;
    }

    bool DiscoverInstallation(std::wstring& detail) {
        for (const std::filesystem::path& candidateRoot : SteamRootCandidates()) {
            if (!IsSupportedDrivePath(candidateRoot)) {
                continue;
            }
            const std::filesystem::path candidateSteam =
                candidateRoot / L"steam.exe";
            std::error_code pathError;
            if (!std::filesystem::is_regular_file(candidateSteam, pathError) ||
                pathError) {
                continue;
            }

            std::vector<std::filesystem::path> libraries;
            AddUniqueRoot(libraries, candidateRoot);
            const std::filesystem::path libraryFile =
                candidateRoot / L"steamapps" / L"libraryfolders.vdf";
            if (const auto text = ReadTextFile(libraryFile); text.has_value()) {
                for (const std::wstring& path : ParseSteamLibraryFolders(*text)) {
                    AddUniqueRoot(libraries, std::filesystem::path(path));
                }
            }

            for (const std::filesystem::path& library : libraries) {
                if (!IsSupportedDrivePath(library)) {
                    continue;
                }
                const std::filesystem::path manifest =
                    library / L"steamapps" / L"appmanifest_289070.acf";
                const auto manifestText = ReadTextFile(manifest);
                if (!manifestText.has_value()) {
                    continue;
                }
                const auto installDir = ParseSteamAppInstallDir(*manifestText);
                if (!installDir.has_value()) {
                    continue;
                }
                const std::filesystem::path installName(*installDir);
                bool unsafe = installName.has_root_path();
                for (const auto& component : installName) {
                    if (component == L"..") {
                        unsafe = true;
                    }
                }
                if (unsafe) {
                    continue;
                }
                const std::filesystem::path candidateGame =
                    library / L"steamapps" / L"common" / installName;
                const std::filesystem::path binaryRoot =
                    candidateGame / kGameBinaryRelative;
                pathError.clear();
                const bool hasDx11 = std::filesystem::is_regular_file(
                    binaryRoot / kDx11Executable, pathError);
                if (pathError) {
                    continue;
                }
                const bool hasDx12 = std::filesystem::is_regular_file(
                    binaryRoot / kDx12Executable, pathError);
                if (pathError || (!hasDx11 && !hasDx12)) {
                    continue;
                }
                steamRoot = candidateRoot;
                steamExecutable = candidateSteam;
                gameRoot = candidateGame;
                return true;
            }
        }
        detail = L"未从 Steam 注册表和 libraryfolders.vdf 找到文明 VI。";
        return false;
    }

    TargetScanResult Scan(bool rejectAnyExisting) const {
        ScopedHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
        if (!snapshot.Valid()) {
            return {TargetScanOutcome::Failed, {}, GetLastError()};
        }
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (!Process32FirstW(snapshot.Get(), &entry)) {
            return {TargetScanOutcome::Failed, {}, GetLastError()};
        }
        do {
            if (!IsCivExecutableName(entry.szExeFile)) {
                continue;
            }
            ScopedHandle candidate(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION |
                                                   SYNCHRONIZE,
                                               FALSE, entry.th32ProcessID));
            if (!candidate.Valid()) {
                return {TargetScanOutcome::Failed, {}, GetLastError()};
            }
            std::vector<wchar_t> pathBuffer(32768);
            DWORD length = static_cast<DWORD>(pathBuffer.size());
            if (!QueryFullProcessImageNameW(candidate.Get(), 0, pathBuffer.data(),
                                            &length)) {
                return {TargetScanOutcome::Failed, {}, GetLastError()};
            }
            std::uint64_t creationTime = 0;
            if (!TryGetProcessCreationTime(candidate.Get(), creationTime)) {
                return {TargetScanOutcome::Failed, {}, GetLastError()};
            }
            const std::filesystem::path imagePath(
                std::wstring(pathBuffer.data(), length));
            const CachedBuild* build = FindBuild(imagePath);
            if (rejectAnyExisting || build != nullptr) {
                return {TargetScanOutcome::Found,
                        {entry.th32ProcessID,
                         build != nullptr ? build->profile : nullptr,
                         imagePath.wstring(), creationTime},
                        ERROR_SUCCESS};
            }
            return {TargetScanOutcome::Found,
                    {entry.th32ProcessID, nullptr, imagePath.wstring(),
                     creationTime},
                    ERROR_SUCCESS};
        } while (Process32NextW(snapshot.Get(), &entry));

        const DWORD error = GetLastError();
        if (error != ERROR_NO_MORE_FILES) {
            return {TargetScanOutcome::Failed, {}, error};
        }
        return {TargetScanOutcome::NotFound, {}, ERROR_SUCCESS};
    }
};

Win32FixPlatform::Win32FixPlatform(Logger& logger)
    : impl_(std::make_unique<Impl>(logger)) {}

Win32FixPlatform::~Win32FixPlatform() = default;

PreparationResult Win32FixPlatform::Prepare() {
    try {
    std::wstring detail;
    if (!impl_->DiscoverInstallation(detail)) {
        impl_->logger.Event("installation_not_found_no_action",
                            "\"detail\":\"" +
                                JsonEscape(Utf8(detail)) + "\"");
        return {PreparationOutcome::InstallationNotFound, std::move(detail)};
    }

    const TargetScanResult existing = impl_->Scan(true);
    if (existing.outcome == TargetScanOutcome::Failed) {
        impl_->logger.Event("process_scan_failed_no_action",
                            "\"win32\":" +
                                std::to_string(existing.win32Error));
        return {PreparationOutcome::ProcessScanFailed,
                L"无法可靠检查文明 VI 是否已经运行。"};
    }
    if (existing.outcome == TargetScanOutcome::Found) {
        impl_->logger.Event("existing_game_detected_no_action",
                            "\"pid\":" +
                                std::to_string(existing.target.pid) +
                                ",\"path\":\"" +
                                JsonEscape(Utf8(existing.target.imagePath)) + "\"");
        return {PreparationOutcome::ExistingGame,
                L"文明 VI 已经运行；为保护当前游戏，本次不附加。"};
    }

    impl_->builds.clear();
    const std::filesystem::path binaryRoot =
        impl_->gameRoot / kGameBinaryRelative;
    bool foundExecutable = false;
    bool hashedExecutable = false;
    bool recognizedBuild = false;
    for (const wchar_t* executableName : {kDx11Executable, kDx12Executable}) {
        const std::filesystem::path path = binaryRoot / executableName;
        std::error_code pathError;
        if (!std::filesystem::is_regular_file(path, pathError) || pathError) {
            continue;
        }
        foundExecutable = true;
        Impl::CachedBuild build;
        build.path = path;
        build.executableName = executableName;
        build.pinnedFile.Reset(CreateFileW(
            path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (build.pinnedFile.Valid()) {
            build.sha256 = Sha256Handle(build.pinnedFile.Get());
            build.fingerprint = FingerprintHandle(build.pinnedFile.Get());
        }
        if (!build.sha256.empty() && build.fingerprint.valid) {
            hashedExecutable = true;
            build.profile = FindBuildProfile(build.executableName, build.sha256);
            recognizedBuild = recognizedBuild || build.profile != nullptr;
        }
        impl_->logger.Event(
            "installed_build_inspected",
            "\"executable\":\"" + JsonEscape(Utf8(build.executableName)) +
                "\",\"path\":\"" + JsonEscape(Utf8(path.wstring())) +
                "\",\"sha256\":\"" + JsonEscape(Utf8(build.sha256)) +
                "\",\"profile\":\"" +
                (build.profile != nullptr
                     ? JsonEscape(Utf8(build.profile->id))
                     : std::string("unknown")) +
                "\",\"support_state\":\"" +
                (build.profile == nullptr
                     ? std::string("unsupported")
                     : std::string(
                           SupportStateLogName(build.profile->supportState))) +
                "\"");
        impl_->builds.push_back(std::move(build));
    }

    if (!foundExecutable) {
        return {PreparationOutcome::InstallationNotFound,
                L"Steam 清单存在，但未找到 DX11 或 DX12 游戏程序。"};
    }
    if (!hashedExecutable) {
        return {PreparationOutcome::HashFailed,
                L"无法读取游戏文件身份；本次不会启动或写入。"};
    }
    if (!recognizedBuild) {
        return {PreparationOutcome::UnsupportedBuild,
                L"当前游戏构建没有匹配的 Build Profile。"};
    }
    return {PreparationOutcome::Ready,
            L"已发现 Steam 安装并完成构建预检。"};
    } catch (const std::exception& error) {
        impl_->logger.Event("preparation_exception_no_action",
                            "\"detail\":\"" +
                                JsonEscape(error.what()) + "\"");
        return {PreparationOutcome::ProcessScanFailed,
                L"Steam 元数据或文件系统状态异常；本次没有启动或写入。"};
    }
}

bool Win32FixPlatform::LaunchViaSteam(std::wstring& detail) {
    const HINSTANCE result = ShellExecuteW(nullptr, L"open",
                                           L"steam://run/289070", nullptr,
                                           nullptr, SW_SHOWNORMAL);
    const auto code = reinterpret_cast<std::intptr_t>(result);
    if (code <= 32) {
        detail = L"Steam 启动请求失败，ShellExecute 错误码 " +
                 std::to_wstring(code) + L"。";
        impl_->logger.Event("steam_launch_failed_no_action",
                            "\"shell_execute\":" +
                                std::to_string(code));
        return false;
    }
    impl_->logger.Event("steam_launch_requested",
                        "\"app_id\":289070,\"listener_already_active\":true");
    return true;
}

TargetScanResult Win32FixPlatform::ScanForTarget() {
    return impl_->Scan(false);
}

RuntimeValidationResult Win32FixPlatform::ValidateRuntime(
    const TargetProcess& target, const BuildProfile& profile) {
    impl_->process.Reset();
    impl_->targetPid = 0;
    impl_->moduleBase = 0;
    impl_->validatedIat = 0;
    impl_->validatedOriginal = 0;
    impl_->activeProfile = nullptr;

    const DWORD readOnlyAccess = PROCESS_QUERY_INFORMATION |
                                 PROCESS_QUERY_LIMITED_INFORMATION |
                                 PROCESS_VM_READ | SYNCHRONIZE;
    DWORD openError = ERROR_SUCCESS;
    for (int attempt = 0; attempt < 1000 && !impl_->process.Valid(); ++attempt) {
        impl_->process.Reset(OpenProcess(readOnlyAccess, FALSE, target.pid));
        if (!impl_->process.Valid()) {
            openError = GetLastError();
            Sleep(5);
        }
    }
    if (!impl_->process.Valid()) {
        return {RuntimeValidationOutcome::ProcessExited, openError,
                L"无法打开目标进程。"};
    }
    impl_->targetPid = target.pid;

    std::vector<wchar_t> pathBuffer(32768);
    DWORD pathLength = static_cast<DWORD>(pathBuffer.size());
    if (!QueryFullProcessImageNameW(impl_->process.Get(), 0, pathBuffer.data(),
                                    &pathLength)) {
        const DWORD error = GetLastError();
        impl_->process.Reset();
        return {RuntimeValidationOutcome::ReadFailed, error,
                L"无法读取目标进程路径。"};
    }
    const std::filesystem::path actualPath(
        std::wstring(pathBuffer.data(), pathLength));
    std::uint64_t runtimeCreationTime = 0;
    const bool creationTimeRead =
        TryGetProcessCreationTime(impl_->process.Get(), runtimeCreationTime);
    const RuntimeProcessIdentityDecision processIdentity =
        DecideRuntimeProcessIdentity({
            GetProcessId(impl_->process.Get()) == target.pid,
            creationTimeRead && target.creationTime != 0 &&
                runtimeCreationTime == target.creationTime,
            EqualsPathInsensitive(actualPath, target.imagePath),
        });
    if (processIdentity != RuntimeProcessIdentityDecision::AcceptSameProcess) {
        impl_->process.Reset();
        return {RuntimeValidationOutcome::ProcessIdentityMismatch,
                ERROR_INVALID_DATA,
                L"目标 PID、创建时间或进程路径与监听阶段不一致。"};
    }
    const Impl::CachedBuild* cached = impl_->FindBuild(actualPath);
    if (cached == nullptr || cached->profile != &profile) {
        impl_->process.Reset();
        return {RuntimeValidationOutcome::ImagePathMismatch, ERROR_INVALID_DATA,
                L"目标进程路径或预检 Profile 已发生变化。"};
    }
    ScopedHandle runtimeFile(CreateFileW(
        actualPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    const FileFingerprint currentFingerprint =
        runtimeFile.Valid() ? FingerprintHandle(runtimeFile.Get())
                            : FileFingerprint{};
    const std::wstring runtimeSha256 =
        runtimeFile.Valid() ? Sha256Handle(runtimeFile.Get()) : std::wstring{};
    if (!runtimeFile.Valid()) {
        impl_->process.Reset();
        return {RuntimeValidationOutcome::ReadFailed, GetLastError(),
                L"无法重新打开运行中的游戏文件做内容校验。"};
    }

    std::optional<RemoteModule> mainModule;
    const std::uint64_t moduleWaitStarted = GetTickCount64();
    while (GetTickCount64() - moduleWaitStarted < kRuntimeWaitMs) {
        mainModule = FindMainModule(target.pid, profile.executableName);
        if (mainModule.has_value()) {
            break;
        }
        if (WaitForSingleObject(impl_->process.Get(), 0) != WAIT_TIMEOUT) {
            impl_->process.Reset();
            return {RuntimeValidationOutcome::ProcessExited, ERROR_PROCESS_ABORTED,
                    L"目标在模块校验前退出。"};
        }
        Sleep(5);
    }
    if (!mainModule.has_value()) {
        impl_->process.Reset();
        return {RuntimeValidationOutcome::ReadFailed, GetLastError(),
                L"未能定位目标主模块。"};
    }

    if (mainModule->size == 0 || mainModule->size != profile.imageSize ||
        !IsAddressAdditionSafe(mainModule->base, mainModule->size - 1U)) {
        impl_->process.Reset();
        return {RuntimeValidationOutcome::PeIdentityMismatch,
                ERROR_INVALID_ADDRESS,
                L"远程主模块范围与 Build Profile 不一致或发生溢出。"};
    }

    const RuntimeFileIdentityDecision fileIdentity = DecideRuntimeFileIdentity({
        SameFingerprint(cached->fingerprint, currentFingerprint),
        currentFingerprint.size == profile.fileSize,
        EqualsInsensitive(runtimeSha256, profile.sha256) &&
            EqualsInsensitive(runtimeSha256, cached->sha256),
        EqualsPathInsensitive(mainModule->path, actualPath),
    });
    if (fileIdentity != RuntimeFileIdentityDecision::AcceptExactIdentity) {
        impl_->process.Reset();
        return {RuntimeValidationOutcome::PeIdentityMismatch, ERROR_INVALID_DATA,
                L"运行时文件哈希、身份或主模块路径与 Build Profile 不一致。"};
    }

    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (!ReadRemote(impl_->process.Get(), mainModule->base, dos) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 ||
        !IsRvaSpanWithinImage(
            profile.imageSize, static_cast<std::uintptr_t>(dos.e_lfanew),
            sizeof(nt)) ||
        !IsCommittedImageSpan(
            impl_->process.Get(),
            mainModule->base + static_cast<std::uintptr_t>(dos.e_lfanew),
            sizeof(nt), mainModule->base, false) ||
        !ReadRemote(impl_->process.Get(),
                    mainModule->base + static_cast<std::uintptr_t>(dos.e_lfanew),
                    nt) ||
        nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.FileHeader.TimeDateStamp != profile.peTimestamp ||
        nt.OptionalHeader.ImageBase != profile.preferredImageBase ||
        nt.OptionalHeader.SizeOfImage != profile.imageSize ||
        nt.OptionalHeader.DllCharacteristics != profile.dllCharacteristics) {
        impl_->process.Reset();
        return {RuntimeValidationOutcome::PeIdentityMismatch, ERROR_INVALID_DATA,
                L"远程 PE 身份与 Build Profile 不一致。"};
    }

    if (!IsRvaSpanWithinImage(profile.imageSize, profile.mtxUnlockIatRva,
                              sizeof(std::uintptr_t)) ||
        !IsRvaSpanWithinImage(profile.imageSize,
                              profile.opensslUnlockReturnRva, 1) ||
        !IsRvaSpanWithinImage(profile.imageSize,
                              profile.opensslLockVectorRva,
                              sizeof(std::uintptr_t) * 2) ||
        !IsRvaSpanWithinImage(profile.imageSize, profile.discoveryGlobalRva,
                              sizeof(std::uintptr_t)) ||
        !IsRvaSpanWithinImage(profile.imageSize, profile.ssoGlobalRva,
                              sizeof(std::uintptr_t))) {
        impl_->process.Reset();
        return {RuntimeValidationOutcome::PeIdentityMismatch,
                ERROR_INVALID_ADDRESS,
                L"Build Profile RVA 区间超出远程映像边界。"};
    }
    if (profile.mutexCountOffset > 0x7FU) {
        impl_->process.Reset();
        return {RuntimeValidationOutcome::MutexLayoutMismatch,
                ERROR_INVALID_DATA,
                L"Build Profile mutex count 偏移无法由 Hook Guard 安全编码。"};
    }

    const std::uintptr_t iatAddress =
        mainModule->base + profile.mtxUnlockIatRva;
    if (!IsCommittedImageSpan(impl_->process.Get(), iatAddress,
                              sizeof(std::uintptr_t), mainModule->base, false) ||
        !IsCommittedImageSpan(
            impl_->process.Get(),
            mainModule->base + profile.opensslUnlockReturnRva, 1,
            mainModule->base, true) ||
        !IsCommittedImageSpan(
            impl_->process.Get(),
            mainModule->base + profile.opensslLockVectorRva,
            sizeof(std::uintptr_t) * 2, mainModule->base, false) ||
        !IsCommittedImageSpan(
            impl_->process.Get(),
            mainModule->base + profile.discoveryGlobalRva,
            sizeof(std::uintptr_t), mainModule->base, false) ||
        !IsCommittedImageSpan(
            impl_->process.Get(), mainModule->base + profile.ssoGlobalRva,
            sizeof(std::uintptr_t), mainModule->base, false)) {
        impl_->process.Reset();
        return {RuntimeValidationOutcome::PeIdentityMismatch,
                ERROR_INVALID_ADDRESS,
                L"Build Profile 远程地址没有落在预期的已提交映像区域。"};
    }
    std::uintptr_t originalTarget = 0;
    std::filesystem::path runtimePath;
    const std::uint64_t iatWaitStarted = GetTickCount64();
    std::uintptr_t lastCandidate = 0;
    std::uintptr_t lastLoggedCandidate = static_cast<std::uintptr_t>(-1);
    std::wstring lastOwner = L"尚未解析";
    for (;;) {
        std::uintptr_t candidate = 0;
        bool exactTargetObserved = false;
        if (ReadRemote(impl_->process.Get(), iatAddress, candidate)) {
            lastCandidate = candidate;
            if (candidate == 0) {
                lastOwner = L"空指针";
            } else if (candidate >= mainModule->base &&
                       candidate - mainModule->base < mainModule->size) {
                lastOwner = L"主映像加载器跳板";
            } else {
                const auto module = FindModuleContaining(target.pid, candidate);
                lastOwner = module.has_value() ? module->name : L"未映射地址";
                if (module.has_value()) {
                    const bool moduleNameMatched =
                        EqualsInsensitive(module->name, L"MSVCP140.dll");
                    const bool systemPathMatched =
                        moduleNameMatched && IsExpectedSystemRuntimePath(
                                                 module->path, L"MSVCP140.dll");
                    const auto exactExport =
                        systemPathMatched
                            ? ResolveRemoteExport(*module, "_Mtx_unlock")
                            : std::optional<std::uintptr_t>{};
                    MEMORY_BASIC_INFORMATION memory{};
                    const bool executableImageMatched =
                        VirtualQueryEx(impl_->process.Get(),
                                       reinterpret_cast<LPCVOID>(candidate), &memory,
                                       sizeof(memory)) == sizeof(memory) &&
                        memory.State == MEM_COMMIT && memory.Type == MEM_IMAGE &&
                        memory.AllocationBase ==
                            reinterpret_cast<PVOID>(module->base) &&
                        IsExecutableProtection(memory.Protect);
                    const RuntimeLibraryIdentityDecision libraryIdentity =
                        DecideRuntimeLibraryIdentity({
                            moduleNameMatched,
                            systemPathMatched,
                            exactExport.has_value() && *exactExport == candidate,
                            executableImageMatched,
                        });
                    if (libraryIdentity ==
                        RuntimeLibraryIdentityDecision::AcceptExactIdentity) {
                        originalTarget = candidate;
                        runtimePath = module->path;
                        exactTargetObserved = true;
                    } else if (moduleNameMatched && !systemPathMatched) {
                        lastOwner = L"MSVCP140.dll（非系统路径）";
                    }
                }
            }
        } else {
            lastOwner = L"不可读";
        }

        const bool waitExpired =
            GetTickCount64() - iatWaitStarted >= kRuntimeWaitMs;
        const RuntimeIatDecision decision =
            DecideRuntimeIatTarget(exactTargetObserved, waitExpired);
        if (decision == RuntimeIatDecision::AcceptExactTarget) {
            break;
        }
        if (candidate != lastLoggedCandidate) {
            impl_->logger.Event(
                "runtime_iat_waiting",
                "\"pid\":" + std::to_string(target.pid) +
                    ",\"observed\":\"" + HexAddress(candidate) +
                    "\",\"owner\":\"" + JsonEscape(Utf8(lastOwner)) +
                    "\",\"writes\":false");
            lastLoggedCandidate = candidate;
        }
        if (decision == RuntimeIatDecision::RejectMismatch) {
            std::wostringstream detail;
            detail << L"等待精确 _Mtx_unlock IAT 超时；最后观察到 "
                   << lastOwner << L"。";
            impl_->process.Reset();
            return {RuntimeValidationOutcome::IatTargetMismatch, WAIT_TIMEOUT,
                    detail.str()};
        }
        if (WaitForSingleObject(impl_->process.Get(), 0) != WAIT_TIMEOUT) {
            impl_->process.Reset();
            return {RuntimeValidationOutcome::ProcessExited, ERROR_PROCESS_ABORTED,
                    L"目标在 IAT 校验前退出。"};
        }
        Sleep(2);
    }

    const std::uintptr_t vectorAddress =
        mainModule->base + profile.opensslLockVectorRva;
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
    if (ReadRemote(impl_->process.Get(), vectorAddress, begin) &&
        ReadRemote(impl_->process.Get(), vectorAddress + sizeof(std::uintptr_t), end) &&
        (begin != 0 || end != 0)) {
        if (!IsPointerVectorIndexAvailable(begin, end,
                                           profile.opensslLockIndex)) {
            impl_->process.Reset();
            return {RuntimeValidationOutcome::MutexLayoutMismatch,
                    ERROR_INVALID_DATA,
                    L"OpenSSL 锁向量布局与 Build Profile 不一致。"};
        }
        std::uintptr_t lock = 0;
        std::int32_t owner = 0;
        std::int32_t count = 0;
        if (!ReadRemote(impl_->process.Get(),
                        begin + profile.opensslLockIndex *
                                    sizeof(std::uintptr_t),
                        lock) ||
            lock == 0 ||
            !IsAddressAdditionSafe(
                lock, (std::max)(profile.mutexOwnerOffset,
                                 profile.mutexCountOffset)) ||
            !ReadRemote(impl_->process.Get(),
                        lock + profile.mutexOwnerOffset, owner) ||
            !ReadRemote(impl_->process.Get(),
                        lock + profile.mutexCountOffset, count)) {
            impl_->process.Reset();
            return {RuntimeValidationOutcome::MutexLayoutMismatch,
                    ERROR_INVALID_DATA,
                    L"无法按 Profile 读取 OpenSSL mutex owner/count。"};
        }
        impl_->logger.Event("mutex_layout_probe",
                            "\"owner\":" + std::to_string(owner) +
                                ",\"count\":" + std::to_string(count));
    }

    const BuildUseDecision buildUse = DecideBuildUse(&profile);
    const bool guardEnabled = BuildUseAllowsGuard(buildUse);
    if (guardEnabled) {
        const DWORD writeAccess = readOnlyAccess | PROCESS_VM_OPERATION |
                                  PROCESS_VM_WRITE;
        ScopedHandle writeProcess(OpenProcess(writeAccess, FALSE, target.pid));
        if (!writeProcess.Valid() ||
            !SameProcessObject(impl_->process.Get(), writeProcess.Get()) ||
            WaitForSingleObject(writeProcess.Get(), 0) != WAIT_TIMEOUT) {
            const DWORD error = GetLastError();
            impl_->process.Reset();
            return {RuntimeValidationOutcome::ReadFailed,
                    error == ERROR_SUCCESS ? ERROR_INVALID_STATE : error,
                    L"只读校验完成，但无法为同一目标建立受限写入句柄。"};
        }
        impl_->process = std::move(writeProcess);
    }

    impl_->moduleBase = mainModule->base;
    impl_->validatedIat = iatAddress;
    impl_->validatedOriginal = originalTarget;
    impl_->activeProfile = &profile;
    impl_->logger.Event(
        "runtime_validated_read_only",
        "\"pid\":" + std::to_string(target.pid) +
            ",\"profile\":\"" + JsonEscape(Utf8(profile.id)) +
            "\",\"module_base\":\"" + HexAddress(mainModule->base) +
            "\",\"iat\":\"" + HexAddress(iatAddress) +
            "\",\"original_unlock\":\"" + HexAddress(originalTarget) +
            "\",\"runtime_path\":\"" +
            JsonEscape(Utf8(runtimePath.wstring())) +
            "\",\"support_state\":\"" +
            SupportStateLogName(profile.supportState) +
            "\",\"write_handle\":" +
            (guardEnabled ? std::string("true") : std::string("false")) +
            ",\"writes\":false");
    return {RuntimeValidationOutcome::Valid, ERROR_SUCCESS,
            L"远程 PE、IAT 和运行库契约校验通过。"};
}

GuardInstallResult Win32FixPlatform::InstallGuard(
    const TargetProcess& target, const BuildProfile& profile) {
    if (!impl_->process.Valid() || impl_->targetPid != target.pid ||
        impl_->activeProfile != &profile ||
        !BuildUseAllowsGuard(DecideBuildUse(&profile)) ||
        impl_->validatedIat == 0 || impl_->validatedOriginal == 0) {
        return {GuardInstallOutcome::FailedNoHook, {}, ERROR_INVALID_STATE,
                L"安装前会话所有权或可写 Profile 状态无效。"};
    }

    std::uintptr_t currentIat = 0;
    if (!ReadRemote(impl_->process.Get(), impl_->validatedIat, currentIat) ||
        currentIat != impl_->validatedOriginal) {
        return {GuardInstallOutcome::FailedNoHook, {}, ERROR_INVALID_STATE,
                L"安装前 IAT 所有权已变化。"};
    }

    void* allocation = VirtualAllocEx(impl_->process.Get(), nullptr, 0x2000,
                                      MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (allocation == nullptr) {
        return {GuardInstallOutcome::FailedNoHook, {}, GetLastError(),
                L"无法为 Hook Guard 分配远程内存。"};
    }
    InstalledGuard guard;
    guard.iatAddress = impl_->validatedIat;
    guard.originalTarget = impl_->validatedOriginal;
    guard.allocationAddress = reinterpret_cast<std::uintptr_t>(allocation);
    guard.guardAddress = guard.allocationAddress;
    guard.counterAddress = guard.allocationAddress + 0x1000;

    const auto code = BuildUnlockGuard(
        impl_->moduleBase + profile.opensslUnlockReturnRva,
        guard.counterAddress, guard.originalTarget,
        static_cast<std::uint8_t>(profile.mutexCountOffset));
    const std::uint64_t zero = 0;
    if (code.size() != 51 ||
        !WriteRemote(impl_->process.Get(), guard.guardAddress, code.data(),
                     code.size()) ||
        !WriteRemote(impl_->process.Get(), guard.counterAddress, &zero,
                     sizeof(zero))) {
        const DWORD error = GetLastError();
        VirtualFreeEx(impl_->process.Get(), allocation, 0, MEM_RELEASE);
        return {GuardInstallOutcome::FailedNoHook, {}, error,
                L"写入 Hook Guard 失败；IAT 尚未修改。"};
    }

    DWORD previousProtection = 0;
    if (!VirtualProtectEx(impl_->process.Get(), allocation, 0x1000,
                          PAGE_EXECUTE_READ, &previousProtection) ||
        !FlushInstructionCache(impl_->process.Get(), allocation, code.size())) {
        const DWORD error = GetLastError();
        VirtualFreeEx(impl_->process.Get(), allocation, 0, MEM_RELEASE);
        return {GuardInstallOutcome::FailedNoHook, {}, error,
                L"设置 Hook Guard 执行权限失败；IAT 尚未修改。"};
    }

    const PatchAttempt patch = PatchPointerChecked(
        impl_->process.Get(), guard.iatAddress, guard.originalTarget,
        guard.guardAddress);
    if (patch.outcome != PointerPatchOutcome::Succeeded) {
        impl_->logger.Event(
            "iat_publish_failed",
            "\"outcome\":\"" + PatchOutcomeName(patch.outcome) +
                "\",\"win32\":" + std::to_string(patch.win32Error) +
                ",\"write_completed\":" +
                (patch.evidence.writeCompleted ? std::string("true")
                                               : std::string("false")) +
                ",\"observed\":\"" + HexAddress(patch.observed) + "\"");
        const FailedPublishDisposition initialDisposition =
            DecideFailedPublishDisposition(
                patch.evidence, patch.observed, guard.originalTarget, false,
                {}, 0);
        if (initialDisposition ==
                FailedPublishDisposition::NoHookSafeToFree ||
            initialDisposition == FailedPublishDisposition::
                                      NoHookProtectionUncertainSafeToFree) {
            VirtualFreeEx(impl_->process.Get(), allocation, 0, MEM_RELEASE);
            if (initialDisposition == FailedPublishDisposition::
                                          NoHookProtectionUncertainSafeToFree) {
                return {
                    GuardInstallOutcome::FailedNoHookProtectionUncertain, {},
                    patch.win32Error,
                    L"Hook Guard 未发布，但 IAT 页面保护未能恢复；请结束本局游戏后再重试。"};
            }
            return {GuardInstallOutcome::FailedNoHook, {}, patch.win32Error,
                    L"IAT 发布失败且回读确认 Hook Guard 从未生效。"};
        }

        PatchAttempt restore;
        const bool restoreAttempted =
            patch.evidence.writeCompleted ||
            !patch.evidence.readBackCompleted ||
            patch.observed == guard.guardAddress;
        if (restoreAttempted) {
            restore = PatchPointerChecked(
                impl_->process.Get(), guard.iatAddress, guard.guardAddress,
                guard.originalTarget, patch.originalProtection);
            impl_->logger.Event(
                "failed_publish_owned_restore",
                "\"outcome\":\"" + PatchOutcomeName(restore.outcome) +
                    "\",\"remote_memory_freed\":false");
        }
        const FailedPublishDisposition finalDisposition =
            DecideFailedPublishDisposition(
                patch.evidence, patch.observed, guard.originalTarget,
                restoreAttempted, restore.evidence, restore.observed);
        if (finalDisposition ==
            FailedPublishDisposition::OriginalRestoredRetainAllocation) {
            return {
                GuardInstallOutcome::FailedRestoredAllocationRetained, guard,
                patch.win32Error,
                L"IAT 发布未完全成功；最终回读确认已恢复原始目标，远程内存按生命周期策略保留。"};
        }
        if (finalDisposition == FailedPublishDisposition::
                                    OriginalRestoredProtectionUncertainRetainAllocation) {
            return {
                GuardInstallOutcome::
                    FailedRestoredProtectionUncertainAllocationRetained,
                guard, patch.win32Error,
                L"IAT 发布未完全成功；最终回读确认已恢复原始目标，但无法证明 IAT 页面保护已恢复，远程内存保留。"};
        }
        return {
            GuardInstallOutcome::FailedStateUncertainAllocationRetained, guard,
            patch.win32Error,
            L"IAT 发布未完全成功，且最终状态无法证明已恢复；未覆盖非本会话所有权，远程内存保留。"};
    }

    impl_->logger.Event(
        "hook_guard_installed",
        "\"pid\":" + std::to_string(target.pid) +
            ",\"iat\":\"" + HexAddress(guard.iatAddress) +
            "\",\"guard\":\"" + HexAddress(guard.guardAddress) +
            "\",\"counter\":\"" + HexAddress(guard.counterAddress) +
            "\",\"hook_lifetime\":\"until_game_exit\"");
    return {GuardInstallOutcome::Installed, guard, ERROR_SUCCESS,
            L"Hook Guard 已安装。"};
}

TargetRunState Win32FixPlatform::QueryTargetState(
    const TargetProcess& target) {
    if (!impl_->process.Valid() || impl_->targetPid != target.pid) {
        return TargetRunState::WaitFailed;
    }
    const DWORD wait = WaitForSingleObject(impl_->process.Get(), 0);
    if (wait == WAIT_TIMEOUT) {
        return TargetRunState::Running;
    }
    if (wait == WAIT_OBJECT_0) {
        return TargetRunState::Exited;
    }
    impl_->logger.Event("target_wait_failed",
                        "\"win32\":" + std::to_string(GetLastError()));
    return TargetRunState::WaitFailed;
}

MonitorSample Win32FixPlatform::ReadMonitorSample(
    const TargetProcess& target, const InstalledGuard& guard) {
    MonitorSample sample;
    if (!impl_->process.Valid() || impl_->targetPid != target.pid ||
        impl_->activeProfile == nullptr || guard.counterAddress == 0 ||
        !ReadRemote(impl_->process.Get(), guard.counterAddress,
                    sample.skippedInvalidUnlocks)) {
        return sample;
    }

    std::uintptr_t discovery = 0;
    if (ReadRemote(impl_->process.Get(),
                   impl_->moduleBase +
                       impl_->activeProfile->discoveryGlobalRva,
                   discovery) &&
        discovery != 0 && IsAddressAdditionSafe(discovery, 0x88U)) {
        ReadRemote(impl_->process.Get(), discovery + 0x88,
                   sample.discoveryState);
    }
    std::uintptr_t sso = 0;
    if (ReadRemote(impl_->process.Get(),
                   impl_->moduleBase + impl_->activeProfile->ssoGlobalRva,
                   sso) &&
        sso != 0 && IsAddressAdditionSafe(sso, 0x3D8U)) {
        ReadRemote(impl_->process.Get(), sso + 0x3D8, sample.ssoState);
    }
    sample.readable = true;
    return sample;
}

HookCleanupOutcome Win32FixPlatform::RestoreOwnedHook(
    const TargetProcess& target, const InstalledGuard& guard) {
    const TargetRunState runState = QueryTargetState(target);
    if (runState == TargetRunState::Exited) {
        return HookCleanupOutcome::TargetExited;
    }
    if (runState != TargetRunState::Running || !impl_->process.Valid() ||
        impl_->targetPid != target.pid) {
        return HookCleanupOutcome::UnreadableNoWrite;
    }

    std::uintptr_t current = 0;
    IatOwnership ownership = IatOwnership::Unreadable;
    if (ReadRemote(impl_->process.Get(), guard.iatAddress, current)) {
        if (current == guard.guardAddress) {
            ownership = IatOwnership::OwnedBySession;
        } else if (current == guard.originalTarget) {
            ownership = IatOwnership::OriginalTarget;
        } else {
            ownership = IatOwnership::ForeignOwner;
        }
    }
    const HookCleanupPlan plan = PlanHookCleanup(true, ownership);
    HookCleanupOutcome outcome = plan.outcome;
    if (plan.restoreOriginalIat) {
        const PatchAttempt restore = PatchPointerChecked(
            impl_->process.Get(), guard.iatAddress, guard.guardAddress,
            guard.originalTarget);
        if (restore.outcome == PointerPatchOutcome::Succeeded) {
            outcome = HookCleanupOutcome::RestoreOwnedAndRetainAllocation;
        } else if (restore.evidence.readBackCompleted &&
                   restore.observed == guard.originalTarget) {
            outcome = HookCleanupOutcome::
                RestorePointerVerifiedProtectionUncertainRetainAllocation;
        } else {
            outcome = HookCleanupOutcome::UnreadableNoWrite;
        }
        impl_->logger.Event(
            "owned_hook_restore",
            "\"outcome\":\"" + PatchOutcomeName(restore.outcome) +
                "\",\"win32\":" + std::to_string(restore.win32Error) +
                ",\"remote_memory_freed\":false");
    } else {
        impl_->logger.Event(
            "hook_restore_skipped",
            "\"observed\":\"" + HexAddress(current) +
                "\",\"remote_memory_freed\":false");
    }
    return outcome;
}

std::uint64_t Win32FixPlatform::MonotonicMilliseconds() {
    return GetTickCount64();
}

void Win32FixPlatform::SleepFor(std::uint32_t milliseconds) {
    Sleep(milliseconds);
}

}  // namespace civ6fix
