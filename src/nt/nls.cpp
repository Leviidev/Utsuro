// nls.cpp -- the national language tables a Windows kernel hands every process through
// the PEB: an ANSI and an OEM code page table and the Unicode case table.
//
// The Xbox image carries none of the c_*.nls files, so the tables are built here in the
// layouts ntdll's RtlInitNlsTables reads: a code page is a header, a 256-entry
// byte-to-wide table, two flag words and a 65536-byte wide-to-byte table; the case table
// is two three-level delta tables.
#include "nt/kernel.h"

#include <cstring>

namespace utsuro::nt {

namespace {

/// A single-byte code page that maps bytes to the same code points and anything above
/// Latin-1 to '?'.
std::vector<uint16_t> BuildCodePage(uint16_t code_page) {
    constexpr size_t kHeader = 13;
    std::vector<uint16_t> w(kHeader + 1 + 258 + 32768, 0);  // 65536 bytes = 32768 words of table
    w[0] = static_cast<uint16_t>(kHeader);
    w[1] = code_page;
    w[2] = 1;      // MaximumCharacterSize
    w[3] = 0x3F;   // DefaultChar
    w[4] = 0x3F;   // UniDefaultChar
    w[5] = 0x3F;   // TransDefaultChar
    w[6] = 0x3F;   // TransUniDefaultChar
    w[kHeader] = 258;  // size of the multibyte section in words
    for (int i = 0; i < 256; ++i) w[kHeader + 1 + i] = static_cast<uint16_t>(i);
    // glyph flag word and DBCS flag word stay zero
    auto* bytes = reinterpret_cast<uint8_t*>(&w[kHeader + 1 + 258]);
    for (int c = 0; c < 65536; ++c) bytes[c] = c < 256 ? static_cast<uint8_t>(c) : 0x3F;
    return w;
}

int16_t Upper(uint32_t c) {
    if (c >= 'a' && c <= 'z') return -32;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return -32;
    if (c == 0xFF) return 121;
    if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return -32;
    if (c >= 0x430 && c <= 0x44F) return -32;
    if (c >= 0x450 && c <= 0x45F) return -80;
    if (c >= 0x100 && c <= 0x17F && c != 0x131 && c != 0x138 && c != 0x149 && c != 0x17F) {
        // Latin Extended-A alternates upper/lower in pairs; the parity flips at 0x139 and 0x179.
        const bool odd_is_lower = !((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E));
        if (odd_is_lower ? (c & 1) : !(c & 1)) return -1;
    }
    return 0;
}

int16_t Lower(uint32_t c) {
    if (c >= 'A' && c <= 'Z') return 32;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return 32;
    if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return 32;
    if (c >= 0x410 && c <= 0x42F) return 32;
    if (c >= 0x400 && c <= 0x40F) return 80;
    if (c >= 0x100 && c <= 0x17F && c != 0x130 && c != 0x138 && c != 0x149 && c != 0x178) {
        const bool odd_is_lower = !((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E));
        if (odd_is_lower ? !(c & 1) : (c & 1)) return 1;
    }
    if (c == 0x178) return -121;
    return 0;
}

/// Three-level table: 256 offsets, 16-entry blocks of offsets, 16-entry blocks of deltas.
std::vector<uint16_t> BuildCaseTable(int16_t (*delta)(uint32_t)) {
    std::vector<uint16_t> t(256, 0);
    // A shared block of 16 zero deltas, and a shared second-level block pointing at it.
    const uint16_t zero3 = static_cast<uint16_t>(t.size());
    t.resize(t.size() + 16, 0);
    const uint16_t zero2 = static_cast<uint16_t>(t.size());
    t.resize(t.size() + 16, zero3);
    for (uint32_t hi = 0; hi < 256; ++hi) {
        bool any = false;
        for (uint32_t lo = 0; lo < 256 && !any; ++lo) any = delta((hi << 8) | lo) != 0;
        if (!any) { t[hi] = zero2; continue; }
        const uint16_t l2 = static_cast<uint16_t>(t.size());
        t.resize(t.size() + 16, zero3);
        t[hi] = l2;
        for (uint32_t mid = 0; mid < 16; ++mid) {
            bool block_any = false;
            for (uint32_t lo = 0; lo < 16 && !block_any; ++lo) block_any = delta((hi << 8) | (mid << 4) | lo) != 0;
            if (!block_any) continue;
            const uint16_t l3 = static_cast<uint16_t>(t.size());
            t.resize(t.size() + 16, 0);
            for (uint32_t lo = 0; lo < 16; ++lo) t[l3 + lo] = static_cast<uint16_t>(delta((hi << 8) | (mid << 4) | lo));
            t[l2 + mid] = l3;
        }
    }
    return t;
}

} // namespace

bool BuildNlsTables(VirtualMemory& vm, uint64_t* ansi, uint64_t* oem, uint64_t* casemap) {
    auto place = [&](const std::vector<uint16_t>& words, uint64_t* out) -> bool {
        uint64_t base = 0, size = words.size() * 2;
        if (vm.Allocate(&base, &size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != STATUS_SUCCESS) return false;
        std::memcpy(reinterpret_cast<void*>(base), words.data(), words.size() * 2);
        *out = base;
        return true;
    };
    if (!place(BuildCodePage(1252), ansi)) return false;
    if (!place(BuildCodePage(437), oem)) return false;
    const auto up = BuildCaseTable(Upper);
    const auto lo = BuildCaseTable(Lower);
    // w[0]: unused by ntdll; w[1]: the upper table's length in words, so the lower table
    // starts at word 2 + w[1].
    std::vector<uint16_t> t(2 + up.size() + lo.size());
    t[0] = 1;
    t[1] = static_cast<uint16_t>(up.size());
    std::memcpy(&t[2], up.data(), up.size() * 2);
    std::memcpy(&t[2 + up.size()], lo.data(), lo.size() * 2);
    return place(t, casemap);
}

} // namespace utsuro::nt
