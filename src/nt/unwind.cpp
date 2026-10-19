// unwind.cpp -- x64 stack unwinding from .pdata, for crash and hang reports.
#include "nt/kernel.h"

#include "nt/safe_mem.h"

#include "engine/log.h"

#include <cstring>

namespace utsuro::nt {

namespace {

struct RuntimeFunction {
    uint32_t begin, end, unwind;
};

/// The function containing `pc` in the image at `base`, or nullptr.
const RuntimeFunction* LookupFunction(uint64_t base, uint64_t pc) {
    const auto* b = reinterpret_cast<const uint8_t*>(base);
    const uint32_t nt = *reinterpret_cast<const uint32_t*>(b + 0x3C);
    const uint32_t rva = *reinterpret_cast<const uint32_t*>(b + nt + 24 + 112 + 3 * 8);
    const uint32_t size = *reinterpret_cast<const uint32_t*>(b + nt + 24 + 112 + 3 * 8 + 4);
    if (rva == 0) return nullptr;
    const auto* table = reinterpret_cast<const RuntimeFunction*>(b + rva);
    const uint32_t count = size / sizeof(RuntimeFunction);
    const uint32_t off = static_cast<uint32_t>(pc - base);
    uint32_t lo = 0, hi = count;
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2;
        if (off < table[mid].begin) hi = mid;
        else if (off >= table[mid].end) lo = mid + 1;
        else return &table[mid];
    }
    return nullptr;
}

} // namespace

std::vector<uint64_t> Kernel::Backtrace(uint64_t rip, const uint64_t* regs_in, int max_frames) {
    std::vector<uint64_t> frames;
    bool bad = false;   // a stack the host cannot read (a thread that has exited, say) ends the walk
    auto rd = [&bad](uint64_t address) -> uint64_t {
        uint64_t value = 0;
        if (!SafeRead(address, &value, 8)) bad = true;
        return value;
    };
    uint64_t regs[16];
    std::memcpy(regs, regs_in, sizeof(regs));
    uint64_t pc = rip;
    for (int depth = 0; depth < max_frames; ++depth) {
        frames.push_back(pc);
        // Which image?
        const auto info = vm_->Query(pc);
        if (info.state != MEM_COMMIT || info.type != MEM_IMAGE) {
            // Not in an image (a call through a bad pointer, or code in a heap): assume the
            // return address is at the top of the stack and carry on from there.
            if (depth == 0 && regs[4] > 0x10000) {
                pc = rd(regs[4]);
                regs[4] += 8;
                if (bad) break;
                continue;
            }
            break;
        }
        const uint64_t base = info.allocation_base;
        const RuntimeFunction* fn = LookupFunction(base, pc);
        uint64_t& rsp = regs[4];
        if (fn == nullptr) {
            // Leaf: the return address is at the top of the stack.
            pc = rd(rsp);
            rsp += 8;
            if (bad) break;
            continue;
        }
        uint32_t offset_in_function = static_cast<uint32_t>(pc - base) - fn->begin;
        const RuntimeFunction* cur = fn;
        bool first = true;
        for (int chain = 0; chain < 8 && cur != nullptr; ++chain) {
            const auto* ui = reinterpret_cast<const uint8_t*>(base + (cur->unwind & ~3u));
            const uint8_t flags = ui[0] >> 3;
            const uint8_t prolog = ui[1];
            const uint8_t ncodes = ui[2];
            const uint8_t frame_reg = ui[3] & 0xF;
            const uint8_t frame_off = ui[3] >> 4;
            const auto* codes = reinterpret_cast<const uint16_t*>(ui + 4);
            // Establish the frame register first when it is in use.
            if (frame_reg != 0 && (!first || offset_in_function >= prolog)) {
                rsp = regs[frame_reg] - static_cast<uint64_t>(frame_off) * 16;
            }
            for (uint32_t i = 0; i < ncodes;) {
                const uint16_t c = codes[i];
                const uint8_t code_offset = c & 0xFF;
                const uint8_t op = (c >> 8) & 0xF;
                const uint8_t opinfo = (c >> 12) & 0xF;
                const bool applies = !first || offset_in_function >= code_offset;
                uint32_t slots = 1;
                switch (op) {
                case 0:  // PUSH_NONVOL
                    if (applies) { regs[opinfo] = rd(rsp); rsp += 8; }
                    break;
                case 1:  // ALLOC_LARGE
                    if (opinfo == 0) { slots = 2; if (applies) rsp += static_cast<uint64_t>(codes[i + 1]) * 8; }
                    else { slots = 3; if (applies) rsp += static_cast<uint64_t>(codes[i + 1]) | (static_cast<uint64_t>(codes[i + 2]) << 16); }
                    break;
                case 2:  // ALLOC_SMALL
                    if (applies) rsp += static_cast<uint64_t>(opinfo) * 8 + 8;
                    break;
                case 3:  // SET_FPREG (handled above)
                    break;
                case 4:  // SAVE_NONVOL
                    slots = 2;
                    if (applies) regs[opinfo] = rd(rsp + static_cast<uint64_t>(codes[i + 1]) * 8);
                    break;
                case 5:  // SAVE_NONVOL_FAR
                    slots = 3;
                    if (applies) regs[opinfo] = rd(rsp + (static_cast<uint64_t>(codes[i + 1]) | (static_cast<uint64_t>(codes[i + 2]) << 16)));
                    break;
                case 8: slots = 2; break;   // SAVE_XMM128
                case 9: slots = 3; break;   // SAVE_XMM128_FAR
                case 10:                    // PUSH_MACHFRAME
                    if (applies) rsp += opinfo ? 48 : 40;
                    break;
                default: break;
                }
                i += slots;
            }
            first = false;
            if (flags & 4) {  // chained
                const uint32_t idx = (ncodes + 1) & ~1u;
                cur = reinterpret_cast<const RuntimeFunction*>(ui + 4 + idx * 2);
                offset_in_function = ~0u;  // everything in a parent applies
            } else {
                cur = nullptr;
            }
        }
        pc = rd(rsp);
        rsp += 8;
        if (bad || pc < 0x10000) break;
    }
    return frames;
}

} // namespace utsuro::nt
