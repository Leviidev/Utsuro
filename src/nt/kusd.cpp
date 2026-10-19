// kusd.cpp -- making images that reference KUSER_SHARED_DATA work where 0x7ffe0000 cannot
// be mapped. See tools/kusd_scan.py for how the tables are made.
#include "nt/kernel.h"

#include "engine/log.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#ifndef UTSURO_DATA_DIR
#define UTSURO_DATA_DIR "data"
#endif

namespace utsuro::nt {

namespace {

int RegisterIndex(const std::string& name) {
    static const char* const q[] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
    static const char* const d[] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d"};
    for (int i = 0; i < 16; ++i) if (name == q[i] || name == d[i]) return i;
    return -1;
}

std::string Lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

void Kernel::ApplyKusdPatches(const PeImage& pe, uint64_t base) {
    if (kusd_ == 0) return;
    std::string name = pe.path.substr(pe.path.rfind('/') + 1);
    char file[512];
    const char* dir = std::getenv("UTSURO_DATA");
    std::snprintf(file, sizeof file, "%s/kusd/%s_%08x_%x.txt", dir ? dir : UTSURO_DATA_DIR, Lower(name).c_str(), pe.timestamp, pe.size_of_image);
    FILE* f = std::fopen(file, "r");
    if (f == nullptr) return;  // this image has no references
    if (kusd_stub_next_ == 0) kusd_stub_next_ = kusd_ + 0x1000;
    int a = 0, b = 0, skipped = 0;
    char line[256];
    while (std::fgets(line, sizeof line, f)) {
        char kind;
        unsigned long long rva;
        int len;
        char reg[160] = {};
        if (std::sscanf(line, "%c %llx %d %159s", &kind, &rva, &len, reg) < 3) continue;
        auto* code = reinterpret_cast<uint8_t*>(base + rva);
        // Offset within KUSER_SHARED_DATA, read back from the instruction itself.
        auto find_offset = [&](int from, int to) -> int64_t {
            for (int i = from; i + 4 <= to; ++i) {
                uint32_t v;
                std::memcpy(&v, code + i, 4);
                if (v >= 0x7FFE0000u && v < 0x7FFE1000u) return static_cast<int64_t>(v - 0x7FFE0000u) | (static_cast<int64_t>(i) << 32);
            }
            return -1;
        };
        if (kind == 'A') {
            // [prefixes][rex] opcode modrm sib(0x25) disp32 [imm]: drop the SIB, make it rip-relative.
            const int64_t found = find_offset(0, len);
            if (found < 0) { ++skipped; continue; }
            const int dpos = static_cast<int>(found >> 32);
            const uint64_t offset = static_cast<uint64_t>(found) & 0xFFFFFFFF;
            if (dpos < 2 || code[dpos - 1] != 0x25 || (code[dpos - 2] & 0xC7) != 0x04) { ++skipped; continue; }
            const int imm = len - (dpos + 4);
            const uint8_t modrm = static_cast<uint8_t>((code[dpos - 2] & 0x38) | 0x05);
            // Everything before the modrm moves up one byte; a nop takes the first byte.
            std::memmove(code + 1, code, static_cast<size_t>(dpos - 2));
            code[0] = 0x90;
            code[dpos - 1] = modrm;
            const int64_t rel = static_cast<int64_t>((kusd_ + offset) - (reinterpret_cast<uint64_t>(code) + static_cast<uint64_t>(len)));
            if (rel > INT32_MAX || rel < INT32_MIN) { UT_ERROR("kusd: %s+%llx is out of rip-relative range", name.c_str(), rva); continue; }
            const int32_t rel32 = static_cast<int32_t>(rel);
            std::memcpy(code + dpos, &rel32, 4);
            (void)imm;
            ++a;
        } else if (kind == 'B') {
            const int r = RegisterIndex(reg);
            const int64_t found = find_offset(0, len);
            if (r < 0 || found < 0 || len < 5) { ++skipped; continue; }
            const uint64_t offset = static_cast<uint64_t>(found) & 0xFFFFFFFF;
            auto* stub = reinterpret_cast<uint8_t*>(kusd_stub_next_);
            if (kusd_stub_next_ + 16 > kusd_ + 0x10000) { UT_ERROR("kusd: out of stub space"); break; }
            stub[0] = static_cast<uint8_t>(0x48 | (r >> 3));
            stub[1] = static_cast<uint8_t>(0xB8 + (r & 7));
            const uint64_t value = kusd_ + offset;
            std::memcpy(stub + 2, &value, 8);
            stub[10] = 0xE9;
            const int64_t back = static_cast<int64_t>((reinterpret_cast<uint64_t>(code) + static_cast<uint64_t>(len)) - (kusd_stub_next_ + 15));
            const int64_t fwd = static_cast<int64_t>(kusd_stub_next_ - (reinterpret_cast<uint64_t>(code) + 5));
            if (back > INT32_MAX || back < INT32_MIN || fwd > INT32_MAX || fwd < INT32_MIN) { UT_ERROR("kusd: %s+%llx is out of jump range", name.c_str(), rva); continue; }
            const int32_t back32 = static_cast<int32_t>(back), fwd32 = static_cast<int32_t>(fwd);
            std::memcpy(stub + 11, &back32, 4);
            code[0] = 0xE9;
            std::memcpy(code + 1, &fwd32, 4);
            for (int i = 5; i < len; ++i) code[i] = 0x90;
            kusd_stub_next_ += 16;
            ++b;
        } else if (kind == 'E' || kind == 'F') {
            const int64_t found = find_offset(0, len);
            if (found < 0 || len < 5) { ++skipped; continue; }
            const uint64_t offset = static_cast<uint64_t>(found) & 0xFFFFFFFF;
            const uint64_t value = kusd_ + offset;
            auto* stub = reinterpret_cast<uint8_t*>(kusd_stub_next_);
            if (kusd_stub_next_ + 48 > kusd_ + 0x10000) { UT_ERROR("kusd: out of stub space"); break; }
            size_t n = 0;
            auto put = [&](std::initializer_list<uint8_t> bytes) { for (uint8_t v : bytes) stub[n++] = v; };
            if (kind == 'E') {
                // mov qword ptr [rsp], imm  ->  push rax; mov rax, imm64; mov [rsp+8], rax; pop rax
                put({0x50, 0x48, 0xB8});
                std::memcpy(stub + n, &value, 8); n += 8;
                put({0x48, 0x89, 0x44, 0x24, 0x08, 0x58});
            } else {
                const int dest = RegisterIndex(reg);
                if (dest < 0) { ++skipped; continue; }
                const int scratch = dest == 1 ? 2 : 1;  // rcx unless that is the destination
                // add dest, imm  ->  push scratch; mov scratch, imm64; add dest, scratch; pop scratch
                put({0x51});                                   // push rcx (or rdx below)
                if (scratch == 2) stub[0] = 0x52;
                put({0x48, 0xB9});
                if (scratch == 2) stub[n - 1] = 0xBA;
                std::memcpy(stub + n, &value, 8); n += 8;
                put({static_cast<uint8_t>(0x48 | ((dest >> 3) ? 0x01 : 0x00)), 0x01, static_cast<uint8_t>(0xC0 | (scratch << 3) | (dest & 7))});
                put({static_cast<uint8_t>(scratch == 2 ? 0x5A : 0x59)});
            }
            stub[n++] = 0xE9;
            const int64_t back = static_cast<int64_t>((reinterpret_cast<uint64_t>(code) + static_cast<uint64_t>(len)) - (kusd_stub_next_ + n + 4));
            const int64_t fwd = static_cast<int64_t>(kusd_stub_next_ - (reinterpret_cast<uint64_t>(code) + 5));
            if (back > INT32_MAX || back < INT32_MIN || fwd > INT32_MAX || fwd < INT32_MIN) { UT_ERROR("kusd: %s+%llx is out of jump range", name.c_str(), rva); continue; }
            const int32_t back32 = static_cast<int32_t>(back), fwd32 = static_cast<int32_t>(fwd);
            std::memcpy(stub + n, &back32, 4); n += 4;
            code[0] = 0xE9;
            std::memcpy(code + 1, &fwd32, 4);
            for (int i = 5; i < len; ++i) code[i] = 0x90;
            kusd_stub_next_ += (n + 15) & ~15ull;
            ++b;
        } else if (kind == 'H') {
            // [base + index*scale + KUSD offset]: compute the address in r11 (saved around it) and run the instruction re-encoded as [r11].
            char base_name[16] = {}, index_name[16] = {}, hex[96] = {};
            int scale = 1;
            unsigned long long offset = 0;
            if (std::sscanf(reg, "%15[^,],%15[^,],%d,%llx,%95s", base_name, index_name, &scale, &offset, hex) != 5) { ++skipped; continue; }
            const int base_reg = std::strcmp(base_name, "-") == 0 ? -1 : RegisterIndex(base_name);
            const int index_reg = std::strcmp(index_name, "-") == 0 ? -1 : RegisterIndex(index_name);
            const size_t new_len = std::strlen(hex) / 2;
            if ((base_reg < 0 && std::strcmp(base_name, "-") != 0) || (index_reg < 0 && std::strcmp(index_name, "-") != 0) || len < 5) { ++skipped; continue; }
            if (kusd_stub_next_ + 96 > kusd_ + 0x10000) { UT_ERROR("kusd: out of stub space"); break; }
            auto* stub = reinterpret_cast<uint8_t*>(kusd_stub_next_);
            size_t n = 0;
            auto put = [&](std::initializer_list<uint8_t> bytes) { for (uint8_t v : bytes) stub[n++] = v; };
            put({0x41, 0x53});                                            // push r11
            put({0x49, 0xBB});                                            // mov r11, imm64
            const uint64_t value = kusd_ + offset;
            std::memcpy(stub + n, &value, 8); n += 8;
            if (base_reg >= 0) put({static_cast<uint8_t>(0x49 | ((base_reg >> 3) << 2)), 0x01, static_cast<uint8_t>(0xC0 | ((base_reg & 7) << 3) | 3)});   // add r11, base
            if (index_reg >= 0) {
                const int log2 = scale == 8 ? 3 : scale == 4 ? 2 : scale == 2 ? 1 : 0;
                put({static_cast<uint8_t>(0x4D | ((index_reg >> 3) << 1)), 0x8D, 0x1C, static_cast<uint8_t>((log2 << 6) | ((index_reg & 7) << 3) | 3)});   // lea r11, [r11 + index*scale]
            }
            for (size_t i = 0; i < new_len; ++i) { unsigned v; std::sscanf(hex + 2 * i, "%2x", &v); stub[n++] = static_cast<uint8_t>(v); }
            put({0x41, 0x5B});                                            // pop r11
            stub[n++] = 0xE9;
            const int64_t back = static_cast<int64_t>((reinterpret_cast<uint64_t>(code) + static_cast<uint64_t>(len)) - (kusd_stub_next_ + n + 4));
            const int64_t fwd = static_cast<int64_t>(kusd_stub_next_ - (reinterpret_cast<uint64_t>(code) + 5));
            if (back > INT32_MAX || back < INT32_MIN || fwd > INT32_MAX || fwd < INT32_MIN) { UT_ERROR("kusd: %s+%llx is out of jump range", name.c_str(), rva); continue; }
            const int32_t back32 = static_cast<int32_t>(back), fwd32 = static_cast<int32_t>(fwd);
            std::memcpy(stub + n, &back32, 4); n += 4;
            code[0] = 0xE9;
            std::memcpy(code + 1, &fwd32, 4);
            for (int i = 5; i < len; ++i) code[i] = 0x90;
            kusd_stub_next_ += (n + 15) & ~15ull;
            ++b;
        } else if (kind == 'G') {
            // A 32-bit operation on a register that now holds a 64-bit address: widen it.
            const int pos = std::atoi(reg);
            if (pos < 0 || pos >= len || (code[pos] & 0xF0) != 0x40) { ++skipped; continue; }
            code[pos] |= 0x08;
            ++b;
        } else {
            ++skipped;
            UT_WARN("kusd: %s+%llx has a reference form that is not rewritten: %s", name.c_str(), rva, line + 0);
        }
    }
    std::fclose(f);
    uint64_t sb = kusd_ + 0x1000, ss = 0xF000;
    vm_->Protect(&sb, &ss, PAGE_EXECUTE_READ, nullptr);
    space_->NotifyContentsReplaced(base, base + pe.size_of_image);
    space_->NotifyContentsReplaced(kusd_ + 0x1000, kusd_ + 0x10000);
    UT_INFO("kusd: %s: %d operands, %d values rewritten, %d left", name.c_str(), a, b, skipped);
}

} // namespace utsuro::nt
