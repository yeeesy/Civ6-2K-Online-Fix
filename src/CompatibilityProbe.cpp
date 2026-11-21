#include "CompatibilityProbe.h"

#include <algorithm>
#include <initializer_list>
#include <string>

namespace civ6fix {
namespace {
struct InvalidImage {};
void Require(bool valid) { if (!valid) throw InvalidImage{}; }

class Image {
public:
    explicit Image(const std::vector<std::uint8_t>& file) : data(file) {
        Require(data.size() >= 64 && data.size() <= 128U * 1024U * 1024U);
        Require(Get<std::uint16_t>(0) == 0x5a4d);
        const auto pe = Get<std::uint32_t>(60);
        Require(pe >= 64 && Get<std::uint32_t>(pe) == 0x4550);
        Require(Get<std::uint16_t>(pe + 4ULL) == 0x8664);
        const auto count = Get<std::uint16_t>(pe + 6ULL);
        const auto optSize = Get<std::uint16_t>(pe + 20ULL);
        const std::size_t opt = pe + 24ULL;
        Require(count > 0 && count <= 96 && optSize >= 240);
        Require(Get<std::uint16_t>(opt) == 0x20b);
        Require(Get<std::uint32_t>(opt + 108) >= 16);
        layout.timestamp = Get<std::uint32_t>(pe + 8ULL);
        layout.imageBase = Get<std::uint64_t>(opt + 24);
        layout.imageSize = Get<std::uint32_t>(opt + 56);
        layout.dllCharacteristics = Get<std::uint16_t>(opt + 70);
        Require(layout.imageSize > 0 && layout.imageSize <= 512U * 1024U * 1024U);
        Require(layout.imageBase == 0x140000000ULL &&
                (layout.dllCharacteristics & 0x120) == 0x120);
        directories = opt + 112;
        for (std::size_t i = 0; i < count; ++i) {
            const auto p = opt + optSize + 40 * i;
            Section s{Get<std::uint32_t>(p + 12), Get<std::uint32_t>(p + 8),
                      Get<std::uint32_t>(p + 20), Get<std::uint32_t>(p + 16),
                      Get<std::uint32_t>(p + 36)};
            s.virtualSize = (std::max)(s.virtualSize, s.rawSize);
            Require(s.va > 0 && s.va < layout.imageSize &&
                    s.virtualSize <= layout.imageSize - s.va);
            Require(s.raw <= data.size() && s.rawSize <= data.size() - s.raw);
            for (const auto& old : sections) {
                Require(s.va + s.virtualSize <= old.va || old.va + old.virtualSize <= s.va);
                Require(!s.rawSize || !old.rawSize ||
                        s.raw + s.rawSize <= old.raw || old.raw + old.rawSize <= s.raw);
            }
            sections.push_back(s);
        }
    }

    template <class T> T Get(std::size_t p) const {
        Require(p <= data.size() && sizeof(T) <= data.size() - p);
        T value{};
        std::memcpy(&value, data.data() + p, sizeof(T));
        return value;
    }
    struct Section { std::uint32_t va, virtualSize, raw, rawSize, flags; };
    const Section& SectionAt(std::uintptr_t rva, std::size_t size, bool raw = true) const {
        Require(size > 0 && IsRvaSpanWithinImage(layout.imageSize, rva, size));
        for (const auto& s : sections) {
            const auto length = raw ? s.rawSize : s.virtualSize;
            if (rva >= s.va && rva - s.va < length && size <= length - (rva - s.va)) return s;
        }
        throw InvalidImage{};
    }
    std::size_t Offset(std::uintptr_t rva, std::size_t size) const {
        const auto& s = SectionAt(rva, size);
        return s.raw + static_cast<std::size_t>(rva - s.va);
    }
    void DataSlot(std::uintptr_t rva, std::size_t size) const {
        const auto& s = SectionAt(rva, size, false);
        Require(rva % 8 == 0 && (s.flags & 0x40000000) && !(s.flags & 0x20000000));
    }
    std::pair<std::uintptr_t, std::size_t> Directory(std::size_t i) const {
        const auto rva = Get<std::uint32_t>(directories + i * 8);
        const auto size = Get<std::uint32_t>(directories + i * 8 + 4);
        Offset(rva, size);
        return {rva, size};
    }
    std::string Name(std::uintptr_t rva) const {
        std::string name;
        for (std::size_t i = 0; i < 512; ++i) {
            const auto c = Get<std::uint8_t>(Offset(rva + i, 1));
            if (!c) return name;
            Require(c < 128);
            name += static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
        }
        throw InvalidImage{};
    }
    std::uintptr_t Relative(std::uintptr_t next, std::size_t displacement) const {
        const auto target = static_cast<std::int64_t>(next) + Get<std::int32_t>(displacement);
        Require(target > 0 && target < layout.imageSize);
        return static_cast<std::uintptr_t>(target);
    }
    bool Match(std::size_t off, std::initializer_list<int> pattern) const {
        if (off > data.size() || pattern.size() > data.size() - off) return false;
        for (const int value : pattern) {
            if (value >= 0 && data[off] != value) return false;
            ++off;
        }
        return true;
    }
    void Code(std::uintptr_t rva, std::size_t size) {
        const auto& s = SectionAt(rva, size);
        Require((s.flags & 0x20000000) && !(s.flags & 0x80000000));
        const auto off = Offset(rva, size);
        layout.code.push_back({rva, {data.begin() + off, data.begin() + off + size}});
    }
    std::pair<std::uintptr_t, std::uintptr_t> Imports() const {
        const auto [rva, length] = Directory(1);
        Require(length <= 20 * 4096);
        std::uintptr_t lock = 0, unlock = 0;
        bool ended = false;
        for (std::size_t q = 0; q + 20 <= length; q += 20) {
            const auto off = Offset(rva + q, 20);
            const auto names = Get<std::uint32_t>(off);
            const auto dll = Get<std::uint32_t>(off + 12);
            const auto iat = Get<std::uint32_t>(off + 16);
            if (!names && !dll && !iat) { ended = true; break; }
            if (Name(dll) != "msvcp140.dll") continue;
            Require(names && iat);
            bool terminated = false;
            for (std::size_t i = 0; i < 8192; ++i) {
                const auto thunk = Get<std::uint64_t>(Offset(names + i * 8ULL, 8));
                if (!thunk) { terminated = true; break; }
                if (thunk >> 63) continue;
                const auto name = Name(thunk + 2);
                if (name != "_mtx_lock" && name != "_mtx_unlock") continue;
                auto& slot = name == "_mtx_lock" ? lock : unlock;
                Require(!slot);
                slot = iat + i * 8ULL;
                DataSlot(slot, 8);
            }
            Require(terminated);
        }
        Require(ended && lock && unlock);
        return {lock, unlock};
    }

    const std::vector<std::uint8_t>& data;
    std::vector<Section> sections;
    CompatibilityLayout layout;
    std::size_t directories = 0;
};
}  // namespace

std::optional<CompatibilityLayout> ProbeCompatibility(const std::vector<std::uint8_t>& file) {
    try {
        Image im(file);
        const auto [lock, unlock] = im.Imports();
        const auto [pdata, pdataSize] = im.Directory(3);
        Require(pdataSize % 12 == 0 && pdataSize <= 12 * 400000);
        std::size_t wrappers = 0;
        for (std::size_t i = 0; i < pdataSize; i += 12) {
            const auto off = im.Offset(pdata + i, 12);
            const auto start = im.Get<std::uint32_t>(off);
            const auto end = im.Get<std::uint32_t>(off + 4);
            if (end < start || end - start < 39 || end - start > 160) continue;
            const auto f = im.Offset(start, end - start);
            if (!im.Match(f, {0x48,0x83,0xec,0x28,0x48,0x8b,0x05}) ||
                !im.Match(f + 11, {0xf6,0xc1,0x01,0x48,0x63,0xca,0x48,0x8b,0x0c,0xc8,0x74,0x08,0xff,0x15}) ||
                !im.Match(f + 29, {0xeb,0x06,0xff,0x15}) || !im.Match(f + 37, {0x85,0xc0})) continue;
            if (im.Relative(start + 29ULL, f + 25) != lock ||
                im.Relative(start + 37ULL, f + 33) != unlock) continue;
            im.layout.lockVector = im.Relative(start + 11ULL, f + 7);
            im.DataSlot(im.layout.lockVector, 24);
            im.layout.unlockIat = unlock;
            im.layout.unlockReturn = start + 37ULL;
            im.Code(start, end - start);
            ++wrappers;
        }
        Require(wrappers == 1);

        // Recognize the two existing service singletons independently of their
        // addresses. Their allocation sizes and constructors constrain state layout.
        std::size_t discoveryFactories = 0, ssoFactories = 0, discoveryReaders = 0;
        for (const auto& s : im.sections) {
            if (!(s.flags & 0x20000000) || (s.flags & 0x80000000)) continue;
            for (std::size_t i = 0; i + 66 <= s.rawSize; ++i) {
                const auto off = s.raw + i;
                const auto rva = s.va + i;
                if (!im.Match(off, {0x48,0x8b,0x3d,-1,-1,-1,-1,0x48,0x85,0xff,0x75,0x34,0xb9}) ||
                    !im.Match(off + 17, {0xe8,-1,-1,-1,-1,0x48,0x89,0x44,0x24,0x78,0x48,0x85,0xc0,0x74,0x0b,0x48,0x8b,0xc8,0xe8}) ||
                    !im.Match(off + 40, {0x48,0x8b,0xf0,0x48,0x8b,0x3d,-1,-1,-1,-1,0x48,0x85,0xff,0x48,0x0f,0x44,0xfe,0x48,0x89,0x3d})) continue;
                const auto size = im.Get<std::uint32_t>(off + 13);
                if (size != 0x2e0 && size != 0x4e8) continue;
                const auto global = im.Relative(rva + 7, off + 3);
                if (im.Relative(rva + 50, off + 46) != global ||
                    im.Relative(rva + 64, off + 60) != global) continue;
                im.DataSlot(global, 8);
                if (size == 0x2e0) {
                    im.layout.discovery = global;
                    ++discoveryFactories;
                } else {
                    const auto ctor = im.Relative(rva + 40, off + 36);
                    const auto c = im.Offset(ctor, 199);
                    // RSI is zero; the qword at 0x3d4 includes the SSO state at 0x3d8.
                    if (!im.Match(c + 44, {0x33,0xf6,0x89,0x77,0x50}) ||
                        !im.Match(c + 153, {0x48,0x89,0x87,0xc0,0x03,0,0,0x48,0x89,0x9f,0xc8,0x03,0,0,0x66,0x89,0xb7,0xd0,0x03,0,0,0x48,0x89,0xb7,0xd4,0x03,0,0})) continue;
                    im.layout.sso = global;
                    im.Code(ctor, 199);
                    ++ssoFactories;
                }
                im.Code(rva, 64);
            }
        }
        Require(discoveryFactories == 1 && ssoFactories == 1);
        // Preserve the known singleton group layout as an additional constraint.
        Require(im.layout.discovery == im.layout.lockVector + 0x98 &&
                im.layout.sso + 0xd0 == im.layout.lockVector);
        for (const auto& s : im.sections) {
            if (!(s.flags & 0x20000000) || (s.flags & 0x80000000)) continue;
            for (std::size_t i = 0; i + 20 <= s.rawSize; ++i) {
                const auto off = s.raw + i;
                const auto rva = s.va + i;
                if (!im.Match(off, {0x48,0x8b,0x0d,-1,-1,-1,-1,0x48,0x85,0xc9,0x75,0x03,0x33,0xc0,0xc3,0xe9}) ||
                    im.Relative(rva + 7, off + 3) != im.layout.discovery) continue;
                const auto getter = im.Relative(rva + 20, off + 16);
                if (!im.Match(im.Offset(getter, 7), {0x8b,0x81,0x88,0,0,0,0xc3})) continue;
                im.Code(rva, 20); im.Code(getter, 7);
                ++discoveryReaders;
            }
        }
        Require(discoveryReaders == 1);
        return std::move(im.layout);
    } catch (const InvalidImage&) { return std::nullopt; }
}

bool MatchesMutexRuntime(const std::array<std::uint8_t, 34>& code) noexcept {
    // Exact decrement/owner-reset ABI; only the RIP-relative call varies.
    constexpr std::uint8_t prefix[]{0x48,0x83,0xec,0x28,0x83,0x69,0x4c,0x01,0x75,0x11,
        0xc7,0x41,0x48,0xff,0xff,0xff,0xff,0x48,0x83,0xc1,0x10,0xff,0x15};
    constexpr std::uint8_t suffix[]{0x33,0xc0,0x48,0x83,0xc4,0x28,0xc3};
    return std::equal(std::begin(prefix), std::end(prefix), code.begin()) &&
           std::equal(std::begin(suffix), std::end(suffix), code.begin() + 27);
}
}  // namespace civ6fix
