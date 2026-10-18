#include "nt/pe.h"

#include "engine/log.h"

#include <cstdio>
#include <cstring>

namespace utsuro::nt {

namespace {

template <typename T>
T Rd(const std::vector<uint8_t>& v, size_t off) {
    T t {};
    if (off + sizeof(T) <= v.size()) std::memcpy(&t, v.data() + off, sizeof(T));
    return t;
}

} // namespace

std::unique_ptr<PeImage> LoadPe(const std::string& path, std::string& error) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        error = "cannot open " + path;
        return nullptr;
    }
    auto pe = std::make_unique<PeImage>();
    pe->path = path;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    pe->file.resize(static_cast<size_t>(size));
    if (std::fread(pe->file.data(), 1, pe->file.size(), f) != pe->file.size()) {
        std::fclose(f);
        error = "short read " + path;
        return nullptr;
    }
    std::fclose(f);

    const auto& b = pe->file;
    if (Rd<uint16_t>(b, 0) != 0x5A4D) { error = "no MZ"; return nullptr; }
    const uint32_t nt = Rd<uint32_t>(b, 0x3C);
    if (Rd<uint32_t>(b, nt) != 0x4550) { error = "no PE signature"; return nullptr; }
    pe->machine = Rd<uint16_t>(b, nt + 4);
    if (pe->machine != 0x8664) { error = "not x64"; return nullptr; }
    const uint16_t nsec = Rd<uint16_t>(b, nt + 6);
    pe->timestamp = Rd<uint32_t>(b, nt + 8);
    const uint16_t opt_size = Rd<uint16_t>(b, nt + 20);
    pe->characteristics = Rd<uint16_t>(b, nt + 22);
    const size_t opt = nt + 24;
    if (Rd<uint16_t>(b, opt) != 0x20B) { error = "not PE32+"; return nullptr; }
    pe->entry_rva = Rd<uint32_t>(b, opt + 16);
    pe->image_base = Rd<uint64_t>(b, opt + 24);
    pe->major_os = Rd<uint16_t>(b, opt + 40);
    pe->minor_os = Rd<uint16_t>(b, opt + 42);
    pe->major_subsystem = Rd<uint16_t>(b, opt + 48);
    pe->minor_subsystem = Rd<uint16_t>(b, opt + 50);
    pe->size_of_image = Rd<uint32_t>(b, opt + 56);
    pe->size_of_headers = Rd<uint32_t>(b, opt + 60);
    pe->checksum = Rd<uint32_t>(b, opt + 64);
    pe->subsystem = Rd<uint16_t>(b, opt + 68);
    pe->dll_characteristics = Rd<uint16_t>(b, opt + 70);
    pe->stack_reserve = Rd<uint64_t>(b, opt + 72);
    pe->stack_commit = Rd<uint64_t>(b, opt + 80);
    pe->heap_reserve = Rd<uint64_t>(b, opt + 88);
    pe->heap_commit = Rd<uint64_t>(b, opt + 96);
    const size_t dd = opt + 112;
    pe->export_rva = Rd<uint32_t>(b, dd + 0);
    pe->export_size = Rd<uint32_t>(b, dd + 4);
    pe->import_rva = Rd<uint32_t>(b, dd + 8);
    pe->import_size = Rd<uint32_t>(b, dd + 12);
    pe->reloc_rva = Rd<uint32_t>(b, dd + 5 * 8);
    pe->reloc_size = Rd<uint32_t>(b, dd + 5 * 8 + 4);

    size_t sh = opt + opt_size;
    for (uint16_t i = 0; i < nsec; ++i, sh += 40) {
        PeSection s;
        std::memcpy(s.name, b.data() + sh, 8);
        s.name[8] = 0;
        s.vsize = Rd<uint32_t>(b, sh + 8);
        s.rva = Rd<uint32_t>(b, sh + 12);
        s.raw_size = Rd<uint32_t>(b, sh + 16);
        s.raw_offset = Rd<uint32_t>(b, sh + 20);
        s.characteristics = Rd<uint32_t>(b, sh + 36);
        pe->sections.push_back(s);
    }
    return pe;
}

bool PlaceImage(const PeImage& pe, uint64_t base, std::string& error) {
    auto* mem = reinterpret_cast<uint8_t*>(base);
    std::memcpy(mem, pe.file.data(), std::min<size_t>(pe.size_of_headers, pe.file.size()));
    for (const auto& s : pe.sections) {
        const uint32_t n = std::min(s.raw_size, s.vsize == 0 ? s.raw_size : s.vsize);
        if (s.raw_offset + n > pe.file.size()) {
            error = "section outside file";
            return false;
        }
        std::memcpy(mem + s.rva, pe.file.data() + s.raw_offset, n);
    }
    if (base == pe.image_base || pe.reloc_size == 0) {
        if (base != pe.image_base && pe.reloc_size == 0) {
            error = "image has no relocations and cannot move";
            return false;
        }
        return true;
    }
    const int64_t delta = static_cast<int64_t>(base - pe.image_base);
    uint32_t off = 0;
    while (off + 8 <= pe.reloc_size) {
        const uint32_t page = *reinterpret_cast<uint32_t*>(mem + pe.reloc_rva + off);
        const uint32_t block = *reinterpret_cast<uint32_t*>(mem + pe.reloc_rva + off + 4);
        if (block < 8) break;
        const uint32_t count = (block - 8) / 2;
        auto* entries = reinterpret_cast<uint16_t*>(mem + pe.reloc_rva + off + 8);
        for (uint32_t i = 0; i < count; ++i) {
            const uint16_t type = entries[i] >> 12;
            const uint16_t where = entries[i] & 0xFFF;
            auto* target = mem + page + where;
            if (type == 10) {  // DIR64
                uint64_t v;
                std::memcpy(&v, target, 8);
                v += static_cast<uint64_t>(delta);
                std::memcpy(target, &v, 8);
            } else if (type == 3) {  // HIGHLOW
                uint32_t v;
                std::memcpy(&v, target, 4);
                v += static_cast<uint32_t>(delta);
                std::memcpy(target, &v, 4);
            } else if (type != 0) {
                UT_WARN("pe: unhandled relocation type %u in %s", type, pe.path.c_str());
            }
        }
        off += block;
    }
    return true;
}

std::map<std::string, uint64_t> ListExports(uint64_t base) {
    std::map<std::string, uint64_t> out;
    auto* mem = reinterpret_cast<const uint8_t*>(base);
    const uint32_t nt = *reinterpret_cast<const uint32_t*>(mem + 0x3C);
    const size_t opt = nt + 24;
    const uint32_t exp_rva = *reinterpret_cast<const uint32_t*>(mem + opt + 112);
    const uint32_t exp_size = *reinterpret_cast<const uint32_t*>(mem + opt + 116);
    if (exp_rva == 0) return out;
    const auto* dir = mem + exp_rva;
    const uint32_t nnames = *reinterpret_cast<const uint32_t*>(dir + 24);
    const auto* funcs = reinterpret_cast<const uint32_t*>(mem + *reinterpret_cast<const uint32_t*>(dir + 28));
    const auto* names = reinterpret_cast<const uint32_t*>(mem + *reinterpret_cast<const uint32_t*>(dir + 32));
    const auto* ords = reinterpret_cast<const uint16_t*>(mem + *reinterpret_cast<const uint32_t*>(dir + 36));
    for (uint32_t i = 0; i < nnames; ++i) {
        const uint32_t rva = funcs[ords[i]];
        if (rva >= exp_rva && rva < exp_rva + exp_size) continue;  // forwarder
        out[reinterpret_cast<const char*>(mem + names[i])] = base + rva;
    }
    return out;
}

uint64_t FindExport(uint64_t base, const char* name) {
    const auto exports = ListExports(base);
    auto it = exports.find(name);
    return it == exports.end() ? 0 : it->second;
}

} // namespace utsuro::nt
