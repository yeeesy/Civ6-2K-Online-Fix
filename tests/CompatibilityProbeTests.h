#pragma once
#include "CompatibilityProbe.h"

namespace compatibility_test {
inline void Put(std::vector<std::uint8_t>& d, std::size_t p, std::uint64_t n, std::size_t width = 4) {
    for (std::size_t i = 0; i < width; ++i) d.at(p + i) = static_cast<std::uint8_t>(n >> (8 * i));
}
inline void Bytes(std::vector<std::uint8_t>& d, std::size_t p, std::initializer_list<int> values) {
    for (int v : values) d.at(p++) = static_cast<std::uint8_t>(v);
}
inline std::size_t Raw(std::size_t rva) { return rva - 0x1000 + 0x400; }
inline void Rel(std::vector<std::uint8_t>& d, std::size_t rva, std::size_t at,
                std::size_t next, std::size_t target) {
    Put(d, Raw(rva) + at, static_cast<std::uint32_t>(target - rva - next));
}
inline std::vector<std::uint8_t> Fixture() {
    // Synthetic PE with no game bytes, paths or proprietary files.
    std::vector<std::uint8_t> d(0x4800);
    Put(d, 0, 0x5a4d, 2); Put(d, 60, 0x80); Put(d, 0x80, 0x4550);
    Put(d, 0x84, 0x8664, 2); Put(d, 0x86, 4, 2); Put(d, 0x88, 1234);
    Put(d, 0x94, 240, 2); Put(d, 0x98, 0x20b, 2);
    Put(d, 0x98 + 24, 0x140000000ULL, 8); Put(d, 0x98 + 56, 0x8000);
    Put(d, 0x98 + 70, 0x8160, 2); Put(d, 0x98 + 108, 16);
    Put(d, 0x98 + 120, 0x6000); Put(d, 0x98 + 124, 40);
    Put(d, 0x98 + 136, 0x7000); Put(d, 0x98 + 140, 12);
    const auto section = [&](std::size_t i, std::size_t va, std::size_t size, std::uint32_t flags) {
        const auto p = 0x188 + i * 40;
        Put(d, p + 8, size); Put(d, p + 12, va); Put(d, p + 16, size);
        Put(d, p + 20, Raw(va)); Put(d, p + 36, flags);
    };
    section(0, 0x1000, 0x2000, 0x60000020);
    section(1, 0x5000, 0x1000, 0xc0000040);
    section(2, 0x6000, 0x1000, 0x40000040);
    section(3, 0x7000, 0x400, 0x40000040);
    // Offset raw data down to avoid overlaps despite the sparse virtual layout.
    d.resize(0x6800);
    Put(d, Raw(0x6000), 0x6100); Put(d, Raw(0x6000) + 12, 0x6200);
    Put(d, Raw(0x6000) + 16, 0x6300);
    Put(d, Raw(0x6100), 0x6220, 8); Put(d, Raw(0x6108), 0x6240, 8);
    const auto name = [&](std::size_t rva, const char* text) {
        std::memcpy(d.data() + Raw(rva), text, std::strlen(text) + 1);
    };
    name(0x6200, "MSVCP140.dll"); name(0x6222, "_Mtx_lock"); name(0x6242, "_Mtx_unlock");
    Put(d, Raw(0x7000), 0x1100); Put(d, Raw(0x7000) + 4, 0x1130);
    Bytes(d, Raw(0x1100), {0x48,0x83,0xec,0x28,0x48,0x8b,0x05}); Rel(d,0x1100,7,11,0x5200);
    Bytes(d, Raw(0x110b), {0xf6,0xc1,1,0x48,0x63,0xca,0x48,0x8b,0x0c,0xc8,0x74,8,0xff,0x15});
    Rel(d,0x1100,25,29,0x6300); Bytes(d,Raw(0x111d),{0xeb,6,0xff,0x15});
    Rel(d,0x1100,33,37,0x6308); Bytes(d,Raw(0x1125),{0x85,0xc0});
    const auto factory = [&](std::size_t rva, std::size_t slot, std::size_t size) {
        Bytes(d,Raw(rva),{0x48,0x8b,0x3d}); Rel(d,rva,3,7,slot);
        Bytes(d,Raw(rva)+7,{0x48,0x85,0xff,0x75,0x34,0xb9}); Put(d,Raw(rva)+13,size);
        Bytes(d,Raw(rva)+17,{0xe8,0,0,0,0,0x48,0x89,0x44,0x24,0x78,0x48,0x85,0xc0,0x74,0x0b,0x48,0x8b,0xc8,0xe8});
        Rel(d,rva,36,40,0x1600);
        Bytes(d,Raw(rva)+40,{0x48,0x8b,0xf0,0x48,0x8b,0x3d}); Rel(d,rva,46,50,slot);
        Bytes(d,Raw(rva)+50,{0x48,0x85,0xff,0x48,0x0f,0x44,0xfe,0x48,0x89,0x3d}); Rel(d,rva,60,64,slot);
    };
    factory(0x1200,0x5130,0x4e8); factory(0x1300,0x5298,0x2e0);
    Bytes(d,Raw(0x162c),{0x33,0xf6,0x89,0x77,0x50});
    Bytes(d,Raw(0x1600)+153,{0x48,0x89,0x87,0xc0,3,0,0,0x48,0x89,0x9f,0xc8,3,0,0,0x66,0x89,0xb7,0xd0,3,0,0,0x48,0x89,0xb7,0xd4,3,0,0});
    Bytes(d,Raw(0x1400),{0x48,0x8b,0x0d}); Rel(d,0x1400,3,7,0x5298);
    Bytes(d,Raw(0x1407),{0x48,0x85,0xc9,0x75,3,0x33,0xc0,0xc3,0xe9}); Rel(d,0x1400,16,20,0x1500);
    Bytes(d,Raw(0x1500),{0x8b,0x81,0x88,0,0,0,0xc3});
    return d;
}
template <class Tests> void Run(Tests& tests) {
    auto file = Fixture();
    auto layout = civ6fix::ProbeCompatibility(file);
    tests.Expect(layout && layout->unlockReturn == 0x1125 && layout->lockVector == 0x5200 &&
                 layout->discovery == 0x5298 && layout->sso == 0x5130,
                 "structural probe locates existing repair and online-state layout without a hash allowlist");
    Put(file,0x88,9876); file[0x6500]=42;
    tests.Expect(civ6fix::ProbeCompatibility(file).has_value(),
                 "timestamp and unrelated EXE changes do not block compatible builds");
    auto moved=file;
    std::copy_n(file.begin()+Raw(0x1100),48,moved.begin()+Raw(0x1800));
    std::fill_n(moved.begin()+Raw(0x1100),48,std::uint8_t{0});
    Rel(moved,0x1800,7,11,0x5200); Rel(moved,0x1800,25,29,0x6300); Rel(moved,0x1800,33,37,0x6308);
    Put(moved,Raw(0x7000),0x1800); Put(moved,Raw(0x7000)+4,0x1830);
    layout=civ6fix::ProbeCompatibility(moved);
    tests.Expect(layout && layout->unlockReturn==0x1825,
                 "moved repair code is resolved by structure instead of old instruction offsets");
    auto bad=file; bad[Raw(0x1120)]=0x90;
    tests.Expect(!civ6fix::ProbeCompatibility(bad),"changed unlock instruction rejects compatibility");
    bad=file; bad[Raw(0x6242)]='X';
    tests.Expect(!civ6fix::ProbeCompatibility(bad),"wrong runtime import rejects compatibility");
    bad=file; Put(bad,0x98+140,24); Put(bad,Raw(0x700c),0x1100); Put(bad,Raw(0x7010),0x1130);
    tests.Expect(!civ6fix::ProbeCompatibility(bad),"ambiguous wrapper rejects compatibility");
    bad=file; bad[Raw(0x1502)]=0x90;
    tests.Expect(!civ6fix::ProbeCompatibility(bad),"changed service field rejects compatibility");
    bad=file; bad[Raw(0x1600)+177]=0x90;
    tests.Expect(!civ6fix::ProbeCompatibility(bad),"changed SSO constructor rejects compatibility");
    bad=file; bad.resize(100);
    tests.Expect(!civ6fix::ProbeCompatibility(bad),"truncated PE rejects compatibility without an out of bounds read");
    std::array<std::uint8_t,34> runtime{0x48,0x83,0xec,0x28,0x83,0x69,0x4c,0x01,0x75,0x11,
        0xc7,0x41,0x48,0xff,0xff,0xff,0xff,0x48,0x83,0xc1,0x10,0xff,0x15,0,0,0,0,
        0x33,0xc0,0x48,0x83,0xc4,0x28,0xc3};
    tests.Expect(civ6fix::MatchesMutexRuntime(runtime),"compatible system mutex layout is recognized");
    runtime[6]=0x50;
    tests.Expect(!civ6fix::MatchesMutexRuntime(runtime),"changed mutex count ABI cannot enable compatibility writes");
}
}  // namespace compatibility_test
