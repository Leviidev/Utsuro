// pe.h -- reading and mapping x64 PE images.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace utsuro::nt {

struct PeSection {
    uint32_t rva {};
    uint32_t vsize {};
    uint32_t raw_offset {};
    uint32_t raw_size {};
    uint32_t characteristics {};
    char name[9] {};
};

/// A parsed image: the headers' facts, and the file bytes to copy sections from.
struct PeImage {
    std::string path;                ///< Host path.
    std::string nt_path;             ///< Name inside the guest, for the loader and queries.
    std::vector<uint8_t> file;
    uint64_t image_base {};
    uint32_t size_of_image {};
    uint32_t size_of_headers {};
    uint32_t entry_rva {};
    uint16_t subsystem {};
    uint16_t dll_characteristics {};
    uint16_t characteristics {};
    uint16_t machine {};
    uint16_t major_os {}, minor_os {}, major_subsystem {}, minor_subsystem {};
    uint64_t stack_reserve {}, stack_commit {}, heap_reserve {}, heap_commit {};
    uint32_t reloc_rva {}, reloc_size {};
    uint32_t export_rva {}, export_size {};
    uint32_t import_rva {}, import_size {};
    uint32_t checksum {};
    uint32_t timestamp {};
    std::vector<PeSection> sections;

    bool IsDll() const { return (characteristics & 0x2000) != 0; }
};

/// Parses `path`. Returns nullptr and fills `error` if it is not an x64 PE.
std::unique_ptr<PeImage> LoadPe(const std::string& path, std::string& error);

/// Copies headers and sections to `base` (memory already mapped read/write and zero),
/// and applies relocations when `base` differs from the preferred address.
bool PlaceImage(const PeImage& pe, uint64_t base, std::string& error);

/// Looks up an export by name in a *mapped* image. Returns 0 if absent.
uint64_t FindExport(uint64_t base, const char* name);

/// Every named export of a mapped image: name -> address.
std::map<std::string, uint64_t> ListExports(uint64_t base);

} // namespace utsuro::nt
