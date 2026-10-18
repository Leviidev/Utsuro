// registry.cpp -- the registry, served from the hives in the dump (SOFTWARE, SYSTEM,
// DEFAULT). Hives are read-only; writes go to an in-memory overlay and are lost on exit.
#include "nt/kernel.h"

#include <fstream>
#include <sstream>

#include "engine/log.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

namespace utsuro::nt {

namespace {

// ---------------------------------------------------------------------------------------
// regf hives
// ---------------------------------------------------------------------------------------

struct Hive {
    std::vector<uint8_t> data;
    uint32_t root {};
    bool valid() const { return !data.empty(); }

    template <typename T>
    T Rd(uint32_t abs) const {
        T t {};
        if (abs + sizeof(T) <= data.size()) std::memcpy(&t, data.data() + abs, sizeof(T));
        return t;
    }
    uint32_t Abs(uint32_t cell) const { return 0x1000 + cell; }
    const uint8_t* At(uint32_t cell) const { return data.data() + Abs(cell); }

    bool Load(const std::string& path) {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (f == nullptr) return false;
        std::fseek(f, 0, SEEK_END);
        data.resize(static_cast<size_t>(std::ftell(f)));
        std::fseek(f, 0, SEEK_SET);
        std::fread(data.data(), 1, data.size(), f);
        std::fclose(f);
        if (data.size() < 0x1020 || std::memcmp(data.data(), "regf", 4) != 0) { data.clear(); return false; }
        root = Rd<uint32_t>(0x24);
        return true;
    }

    std::string Name(uint32_t cell) const {
        const uint32_t a = Abs(cell) + 4;  // skip the cell size
        const uint16_t flags = Rd<uint16_t>(a + 2);
        const uint16_t len = Rd<uint16_t>(a + 0x48);
        std::string out;
        if (flags & 0x20) {
            for (uint16_t i = 0; i < len; ++i) out += static_cast<char>(Rd<uint8_t>(a + 0x4C + i));
        } else {
            std::u16string u;
            for (uint16_t i = 0; i + 1 < len; i += 2) u += static_cast<char16_t>(Rd<uint16_t>(a + 0x4C + i));
            out = Utf16ToUtf8(u);
        }
        return out;
    }

    std::vector<uint32_t> Children(uint32_t cell) const {
        std::vector<uint32_t> out;
        const uint32_t a = Abs(cell) + 4;
        const uint32_t count = Rd<uint32_t>(a + 0x14);
        const uint32_t list = Rd<uint32_t>(a + 0x1C);
        if (count == 0 || list == 0xFFFFFFFF) return out;
        CollectList(list, out);
        return out;
    }

    void CollectList(uint32_t list, std::vector<uint32_t>& out) const {
        const uint32_t a = Abs(list) + 4;
        const char* sig = reinterpret_cast<const char*>(data.data() + a);
        const uint16_t n = Rd<uint16_t>(a + 2);
        if (!std::strncmp(sig, "lf", 2) || !std::strncmp(sig, "lh", 2)) {
            for (uint16_t i = 0; i < n; ++i) out.push_back(Rd<uint32_t>(a + 4 + i * 8));
        } else if (!std::strncmp(sig, "li", 2)) {
            for (uint16_t i = 0; i < n; ++i) out.push_back(Rd<uint32_t>(a + 4 + i * 4));
        } else if (!std::strncmp(sig, "ri", 2)) {
            for (uint16_t i = 0; i < n; ++i) CollectList(Rd<uint32_t>(a + 4 + i * 4), out);
        }
    }

    struct Value {
        std::string name;
        uint32_t type {};
        std::vector<uint8_t> data;
    };

    std::vector<Value> Values(uint32_t cell) const {
        std::vector<Value> out;
        const uint32_t a = Abs(cell) + 4;
        const uint32_t count = Rd<uint32_t>(a + 0x24);
        const uint32_t list = Rd<uint32_t>(a + 0x28);
        if (count == 0 || list == 0xFFFFFFFF) return out;
        const uint32_t la = Abs(list) + 4;
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t vk = Rd<uint32_t>(la + i * 4);
            const uint32_t va = Abs(vk) + 4;
            if (std::memcmp(data.data() + va, "vk", 2) != 0) continue;
            Value v;
            const uint16_t nlen = Rd<uint16_t>(va + 2);
            uint32_t dlen = Rd<uint32_t>(va + 4);
            const uint32_t doff = Rd<uint32_t>(va + 8);
            v.type = Rd<uint32_t>(va + 0xC);
            const uint16_t flags = Rd<uint16_t>(va + 0x10);
            if (flags & 1) {
                for (uint16_t k = 0; k < nlen; ++k) v.name += static_cast<char>(Rd<uint8_t>(va + 0x14 + k));
            } else {
                std::u16string u;
                for (uint16_t k = 0; k + 1 < nlen; k += 2) u += static_cast<char16_t>(Rd<uint16_t>(va + 0x14 + k));
                v.name = Utf16ToUtf8(u);
            }
            if (dlen & 0x80000000u) {
                dlen &= 0x7FFFFFFFu;
                v.data.assign(reinterpret_cast<const uint8_t*>(&data[va + 8]), reinterpret_cast<const uint8_t*>(&data[va + 8]) + std::min<uint32_t>(dlen, 4));
            } else if (dlen != 0) {
                const uint32_t da = Abs(doff) + 4;
                if (!std::memcmp(data.data() + da, "db", 2)) {
                    // Big data: a list of segments of up to 16344 bytes.
                    const uint16_t segs = Rd<uint16_t>(da + 2);
                    const uint32_t seglist = Abs(Rd<uint32_t>(da + 4)) + 4;
                    for (uint16_t s = 0; s < segs && v.data.size() < dlen; ++s) {
                        const uint32_t sa = Abs(Rd<uint32_t>(seglist + s * 4)) + 4;
                        const uint32_t take = std::min<uint32_t>(16344, dlen - static_cast<uint32_t>(v.data.size()));
                        v.data.insert(v.data.end(), data.begin() + sa, data.begin() + sa + take);
                    }
                } else if (da + dlen <= data.size()) {
                    v.data.assign(data.begin() + da, data.begin() + da + dlen);
                }
            }
            out.push_back(std::move(v));
        }
        return out;
    }

    uint64_t LastWrite(uint32_t cell) const { return Rd<uint64_t>(Abs(cell) + 4 + 4); }
};

std::string Lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// ---------------------------------------------------------------------------------------
// The tree: hives mounted under \Registry, with an overlay for changes.
// ---------------------------------------------------------------------------------------

struct Node {
    bool exists {false};
    Hive* hive {nullptr};   ///< nullptr for a virtual node or an overlay-only key
    uint32_t cell {0};
    std::string path;       ///< Normalised, lower case, no leading backslash.
};

struct Overlay {
    bool created {false};
    std::string display_name;   ///< The key's name as it was created, with its case.
    std::map<std::string, Hive::Value> values;   // key: lower-cased value name
    std::set<std::string> deleted_values;
};

class Registry {
public:
    void Init(const std::string& config_dir) {
        software_.Load(config_dir + "/SOFTWARE");
        system_.Load(config_dir + "/SYSTEM");
        default_.Load(config_dir + "/DEFAULT");
        UT_INFO("registry: SOFTWARE %s, SYSTEM %s, DEFAULT %s", software_.valid() ? "loaded" : "missing",
                system_.valid() ? "loaded" : "missing", default_.valid() ? "loaded" : "missing");
    }

    /// Splits an NT registry path into lower-case components below \Registry.
    static std::vector<std::string> Split(const std::string& path) {
        std::vector<std::string> parts;
        std::string cur;
        for (char c : path) {
            if (c == '\\' || c == '/') {
                if (!cur.empty()) parts.push_back(Lower(cur));
                cur.clear();
            } else {
                cur += c;
            }
        }
        if (!cur.empty()) parts.push_back(Lower(cur));
        if (!parts.empty() && parts[0] == "registry") parts.erase(parts.begin());
        return parts;
    }

    Node Resolve(const std::vector<std::string>& parts) {
        Node n;
        for (size_t i = 0; i < parts.size(); ++i) n.path += (i ? "\\" : "") + parts[i];
        std::scoped_lock lock {mutex_};
        if (parts.empty()) { n.exists = true; return n; }
        Hive* hive = nullptr;
        size_t index = 1;
        if (parts[0] == "machine") {
            if (parts.size() == 1) { n.exists = true; return n; }
            if (parts[1] == "software") hive = &software_;
            else if (parts[1] == "system") hive = &system_;
            else {
                // hardware, sam, security... exist but are empty.
                n.exists = overlay_.count(n.path) != 0 || parts.size() == 2;
                return n;
            }
            index = 2;
        } else if (parts[0] == "user") {
            if (parts.size() == 1) { n.exists = true; return n; }
            hive = &default_;  // every user is the default user
            index = 2;
        } else {
            return n;
        }
        if (hive == nullptr || !hive->valid()) { n.exists = overlay_.count(n.path) != 0; return n; }
        uint32_t cell = hive->root;
        for (size_t i = index; i < parts.size(); ++i) {
            std::string want = parts[i];
            if (hive == &system_ && i == index && want == "currentcontrolset") want = "controlset001";
            uint32_t next = 0xFFFFFFFF;
            for (uint32_t child : hive->Children(cell)) {
                if (Lower(hive->Name(child)) == want) { next = child; break; }
            }
            if (next == 0xFFFFFFFF) {
                n.exists = overlay_.count(n.path) != 0 && overlay_[n.path].created;
                n.hive = nullptr;
                return n;
            }
            cell = next;
        }
        n.exists = true;
        n.hive = hive;
        n.cell = cell;
        return n;
    }

    std::vector<std::string> ChildNames(const Node& n) {
        std::vector<std::string> out;
        std::scoped_lock lock {mutex_};
        if (n.hive != nullptr) {
            for (uint32_t c : n.hive->Children(n.cell)) out.push_back(n.hive->Name(c));
        } else if (n.path == "machine") {
            out = {"HARDWARE", "SAM", "SECURITY", "SOFTWARE", "SYSTEM"};
        } else if (n.path == "user") {
            out = {".DEFAULT"};
        }
        for (const auto& [path, ov] : overlay_) {
            if (!ov.created) continue;
            const auto slash = path.rfind('\\');
            if (slash != std::string::npos && path.substr(0, slash) == n.path) out.push_back(path.substr(slash + 1));
        }
        return out;
    }

    std::vector<Hive::Value> ValuesOf(const Node& n) {
        std::vector<Hive::Value> out;
        std::scoped_lock lock {mutex_};
        if (n.hive != nullptr) out = n.hive->Values(n.cell);
        auto it = overlay_.find(n.path);
        if (it != overlay_.end()) {
            for (const auto& d : it->second.deleted_values) {
                out.erase(std::remove_if(out.begin(), out.end(), [&](const Hive::Value& v) { return Lower(v.name) == d; }), out.end());
            }
            for (const auto& [k, v] : it->second.values) {
                bool replaced = false;
                for (auto& e : out) if (Lower(e.name) == k) { e = v; replaced = true; }
                if (!replaced) out.push_back(v);
            }
        }
        return out;
    }

    Overlay& OverlayFor(const std::string& path) {
        std::scoped_lock lock {mutex_};
        return overlay_[path];
    }

    uint64_t LastWrite(const Node& n) {
        if (n.hive != nullptr) return n.hive->LastWrite(n.cell);
        return static_cast<uint64_t>(Kernel::NtSystemTime());
    }

    std::string NameOf(const Node& n) {
        if (n.hive != nullptr) return n.hive->Name(n.cell);
        {
            std::scoped_lock lock {mutex_};
            auto it = overlay_.find(n.path);
            if (it != overlay_.end() && !it->second.display_name.empty()) return it->second.display_name;
        }
        const auto slash = n.path.rfind('\\');
        return slash == std::string::npos ? n.path : n.path.substr(slash + 1);
    }

    uint32_t SubkeyCount(const Node& n) { return static_cast<uint32_t>(ChildNames(n).size()); }

private:
    std::mutex mutex_;
    Hive software_, system_, default_;
    std::map<std::string, Overlay> overlay_;
};

Registry g_registry;

std::vector<std::string> PathFromAttributes(Kernel& k, const OBJECT_ATTRIBUTES* oa) {
    if (oa == nullptr || oa->ObjectName == 0) return {};
    std::string name = Utf16ToUtf8(ReadUnicodeString(reinterpret_cast<const UNICODE_STRING*>(oa->ObjectName)));
    std::vector<std::string> parts;
    if (oa->RootDirectory != 0) {
        auto root = k.Get<KeyObject>(oa->RootDirectory, ObjType::Key);
        if (root) parts = Registry::Split(root->path);
    }
    for (auto& p : Registry::Split(name)) parts.push_back(p);
    // "\Registry" prefixes only count at the start.
    return parts;
}

NTSTATUS DoOpen(Kernel& k, NtThread&, SyscallArgs& a, bool create) {
    const auto parts = PathFromAttributes(k, a.Ptr<OBJECT_ATTRIBUTES>(2));
    std::vector<std::string> full = parts;
    if (!full.empty() && full[0] == "registry") full.erase(full.begin());
    Node n = g_registry.Resolve(full);
    uint32_t disposition = 2;  // REG_OPENED_EXISTING_KEY
    if (!n.exists) {
        if (!create) {
            UT_DEBUG("registry: %s not found", n.path.c_str());
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }
        g_registry.OverlayFor(n.path).created = true;
        n.exists = true;
        disposition = 1;
    }
    auto key = std::make_shared<KeyObject>();
    key->path = n.path;
    key->name = n.path;
    *a.Ptr<HANDLE>(0) = k.AddHandle(key);
    if (create) {
        if (auto* d = a.Ptr<uint32_t>(6)) *d = disposition;
    }
    UT_DEBUG("registry: opened %s", n.path.c_str());
    return STATUS_SUCCESS;
}

NTSTATUS NtOpenKey(Kernel& k, NtThread& t, SyscallArgs& a) { return DoOpen(k, t, a, false); }
NTSTATUS NtCreateKey(Kernel& k, NtThread& t, SyscallArgs& a) { return DoOpen(k, t, a, true); }

NTSTATUS PutName(uint8_t* buf, uint32_t len, uint32_t fixed, const std::string& name, uint32_t name_len_off, uint32_t* ret) {
    const std::u16string u = Utf8ToUtf16(name);
    const uint32_t bytes = static_cast<uint32_t>(u.size() * 2);
    if (ret) *ret = fixed + bytes;
    if (len < fixed) return STATUS_BUFFER_TOO_SMALL;
    *reinterpret_cast<uint32_t*>(buf + name_len_off) = bytes;
    const uint32_t room = len - fixed;
    std::memcpy(buf + fixed, u.data(), std::min(room, bytes));
    return room < bytes ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
}

// NtQueryKey(Key, Class, Buffer, Length, PULONG Ret)
NTSTATUS NtQueryKey(Kernel& k, NtThread&, SyscallArgs& a) {
    auto key = k.Get<KeyObject>(a[0], ObjType::Key);
    if (!key) return STATUS_INVALID_HANDLE;
    const uint32_t cls = static_cast<uint32_t>(a[1]);
    auto* buf = a.Ptr<uint8_t>(2);
    const uint32_t len = static_cast<uint32_t>(a[3]);
    auto* ret = a.Ptr<uint32_t>(4);
    const Node n = g_registry.Resolve(Registry::Split(key->path));
    if (!n.exists) return STATUS_KEY_NOT_FOUND;
    const auto names = g_registry.ChildNames(n);
    const auto values = g_registry.ValuesOf(n);
    switch (cls) {
    case 0: {  // KeyBasicInformation
        std::memset(buf, 0, std::min<uint32_t>(len, 16));
        if (len >= 8) *reinterpret_cast<uint64_t*>(buf) = g_registry.LastWrite(n);
        return PutName(buf, len, 16, g_registry.NameOf(n), 12, ret);
    }
    case 1: {  // KeyNodeInformation
        std::memset(buf, 0, std::min<uint32_t>(len, 20));
        if (len >= 8) *reinterpret_cast<uint64_t*>(buf) = g_registry.LastWrite(n);
        if (len >= 16) *reinterpret_cast<uint32_t*>(buf + 12) = 0;
        return PutName(buf, len, 20, g_registry.NameOf(n), 16, ret);
    }
    case 2: {  // KeyFullInformation
        if (ret) *ret = 44;
        if (len < 44) return STATUS_BUFFER_TOO_SMALL;
        std::memset(buf, 0, 44);
        uint32_t max_name = 0, max_value_name = 0, max_data = 0;
        for (auto& c : names) max_name = std::max<uint32_t>(max_name, static_cast<uint32_t>(c.size() * 2));
        for (auto& v : values) { max_value_name = std::max<uint32_t>(max_value_name, static_cast<uint32_t>(v.name.size() * 2)); max_data = std::max<uint32_t>(max_data, static_cast<uint32_t>(v.data.size())); }
        *reinterpret_cast<uint64_t*>(buf) = g_registry.LastWrite(n);
        *reinterpret_cast<uint32_t*>(buf + 12) = 44;  // ClassOffset
        *reinterpret_cast<uint32_t*>(buf + 20) = static_cast<uint32_t>(names.size());
        *reinterpret_cast<uint32_t*>(buf + 24) = max_name;
        *reinterpret_cast<uint32_t*>(buf + 32) = static_cast<uint32_t>(values.size());
        *reinterpret_cast<uint32_t*>(buf + 36) = max_value_name;
        *reinterpret_cast<uint32_t*>(buf + 40) = max_data;
        return STATUS_SUCCESS;
    }
    case 3: {  // KeyNameInformation
        std::string full = "\\REGISTRY";
        for (auto& p : Registry::Split(key->path)) full += "\\" + p;
        return PutName(buf, len, 4, full, 0, ret);
    }
    case 4: {  // KeyCachedInformation
        if (ret) *ret = 40;
        if (len < 40) return STATUS_BUFFER_TOO_SMALL;
        std::memset(buf, 0, 40);
        *reinterpret_cast<uint64_t*>(buf) = g_registry.LastWrite(n);
        *reinterpret_cast<uint32_t*>(buf + 12) = static_cast<uint32_t>(names.size());
        *reinterpret_cast<uint32_t*>(buf + 28) = static_cast<uint32_t>(values.size());
        return STATUS_SUCCESS;
    }
    default:
        UT_DEBUG("NtQueryKey class %u not handled", cls);
        return STATUS_INVALID_INFO_CLASS;
    }
}

// NtEnumerateKey(Key, Index, Class, Buffer, Length, PULONG Ret)
NTSTATUS NtEnumerateKey(Kernel& k, NtThread&, SyscallArgs& a) {
    auto key = k.Get<KeyObject>(a[0], ObjType::Key);
    if (!key) return STATUS_INVALID_HANDLE;
    const uint32_t index = static_cast<uint32_t>(a[1]);
    const uint32_t cls = static_cast<uint32_t>(a[2]);
    auto* buf = a.Ptr<uint8_t>(3);
    const uint32_t len = static_cast<uint32_t>(a[4]);
    auto* ret = a.Ptr<uint32_t>(5);
    const Node n = g_registry.Resolve(Registry::Split(key->path));
    const auto names = g_registry.ChildNames(n);
    if (index >= names.size()) return STATUS_NO_MORE_ENTRIES;
    if (cls == 0) {
        std::memset(buf, 0, std::min<uint32_t>(len, 16));
        return PutName(buf, len, 16, names[index], 12, ret);
    }
    if (cls == 1) {
        std::memset(buf, 0, std::min<uint32_t>(len, 20));
        return PutName(buf, len, 20, names[index], 16, ret);
    }
    if (cls == 3) return PutName(buf, len, 4, names[index], 0, ret);
    return STATUS_INVALID_INFO_CLASS;
}

NTSTATUS FillValue(const Hive::Value& v, uint32_t cls, uint8_t* buf, uint32_t len, uint32_t* ret) {
    const std::u16string uname = Utf8ToUtf16(v.name);
    const uint32_t nbytes = static_cast<uint32_t>(uname.size() * 2);
    switch (cls) {
    case 0: {  // KeyValueBasicInformation
        const uint32_t need = 12 + nbytes;
        if (ret) *ret = need;
        if (len < 12) return STATUS_BUFFER_TOO_SMALL;
        *reinterpret_cast<uint32_t*>(buf) = 0;
        *reinterpret_cast<uint32_t*>(buf + 4) = v.type;
        *reinterpret_cast<uint32_t*>(buf + 8) = nbytes;
        std::memcpy(buf + 12, uname.data(), std::min<uint32_t>(len - 12, nbytes));
        return len < need ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
    }
    case 1: {  // KeyValueFullInformation
        const uint32_t data_off = (20 + nbytes + 7) & ~7u;
        const uint32_t need = data_off + static_cast<uint32_t>(v.data.size());
        if (ret) *ret = need;
        if (len < 20) return STATUS_BUFFER_TOO_SMALL;
        *reinterpret_cast<uint32_t*>(buf) = 0;
        *reinterpret_cast<uint32_t*>(buf + 4) = v.type;
        *reinterpret_cast<uint32_t*>(buf + 8) = data_off;
        *reinterpret_cast<uint32_t*>(buf + 12) = static_cast<uint32_t>(v.data.size());
        *reinterpret_cast<uint32_t*>(buf + 16) = nbytes;
        std::memcpy(buf + 20, uname.data(), std::min<uint32_t>(len - 20, nbytes));
        if (len > data_off) std::memcpy(buf + data_off, v.data.data(), std::min<size_t>(len - data_off, v.data.size()));
        return len < need ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
    }
    case 3: {  // KeyValueFullInformationAlign64: Type, DataOffset, DataLength, NameLength, Name, Data
        const uint32_t data_off = (16 + nbytes + 7) & ~7u;
        const uint32_t need = data_off + static_cast<uint32_t>(v.data.size());
        if (ret) *ret = need;
        if (len < 16) return STATUS_BUFFER_TOO_SMALL;
        *reinterpret_cast<uint32_t*>(buf) = v.type;
        *reinterpret_cast<uint32_t*>(buf + 4) = data_off;
        *reinterpret_cast<uint32_t*>(buf + 8) = static_cast<uint32_t>(v.data.size());
        *reinterpret_cast<uint32_t*>(buf + 12) = nbytes;
        std::memcpy(buf + 16, uname.data(), std::min<uint32_t>(len - 16, nbytes));
        if (len > data_off) std::memcpy(buf + data_off, v.data.data(), std::min<size_t>(len - data_off, v.data.size()));
        return len < need ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
    }
    case 2:    // KeyValuePartialInformation: TitleIndex, Type, DataLength, Data
    case 4: {  // KeyValuePartialInformationAlign64: Type, DataLength, Data
        const uint32_t head = cls == 2 ? 12 : 8;
        const uint32_t need = head + static_cast<uint32_t>(v.data.size());
        if (ret) *ret = need;
        if (len < head) return STATUS_BUFFER_TOO_SMALL;
        if (cls == 2) {
            *reinterpret_cast<uint32_t*>(buf) = 0;
            *reinterpret_cast<uint32_t*>(buf + 4) = v.type;
            *reinterpret_cast<uint32_t*>(buf + 8) = static_cast<uint32_t>(v.data.size());
        } else {
            *reinterpret_cast<uint32_t*>(buf) = v.type;
            *reinterpret_cast<uint32_t*>(buf + 4) = static_cast<uint32_t>(v.data.size());
        }
        std::memcpy(buf + head, v.data.data(), std::min<size_t>(len - head, v.data.size()));
        return len < need ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
    }
    default:
        return STATUS_INVALID_INFO_CLASS;
    }
}

// NtQueryValueKey(Key, PUNICODE_STRING Name, Class, Buffer, Length, PULONG Ret)
NTSTATUS NtQueryValueKey(Kernel& k, NtThread&, SyscallArgs& a) {
    auto key = k.Get<KeyObject>(a[0], ObjType::Key);
    if (!key) return STATUS_INVALID_HANDLE;
    const std::string want = Lower(Utf16ToUtf8(ReadUnicodeString(a.Ptr<UNICODE_STRING>(1))));
    const Node n = g_registry.Resolve(Registry::Split(key->path));
    for (const auto& v : g_registry.ValuesOf(n)) {
        if (Lower(v.name) == want) {
            std::string text;
            if (v.type == 1 || v.type == 2) text = Utf16ToUtf8(std::u16string(reinterpret_cast<const char16_t*>(v.data.data()), v.data.size() / 2));
            else if (v.type == 4 && v.data.size() >= 4) text = std::to_string(*reinterpret_cast<const uint32_t*>(v.data.data()));
            UT_DEBUG("registry: %s\\%s = (type %u) %s", key->path.c_str(), want.c_str(), v.type, text.c_str());
            return FillValue(v, static_cast<uint32_t>(a[2]), a.Ptr<uint8_t>(3), static_cast<uint32_t>(a[4]), a.Ptr<uint32_t>(5));
        }
    }
    UT_DEBUG("registry: value '%s' not found in %s", want.c_str(), key->path.c_str());
    return STATUS_OBJECT_NAME_NOT_FOUND;
}

// NtEnumerateValueKey(Key, Index, Class, Buffer, Length, PULONG Ret)
NTSTATUS NtEnumerateValueKey(Kernel& k, NtThread&, SyscallArgs& a) {
    auto key = k.Get<KeyObject>(a[0], ObjType::Key);
    if (!key) return STATUS_INVALID_HANDLE;
    const Node n = g_registry.Resolve(Registry::Split(key->path));
    const auto values = g_registry.ValuesOf(n);
    const uint32_t index = static_cast<uint32_t>(a[1]);
    if (index >= values.size()) return STATUS_NO_MORE_ENTRIES;
    return FillValue(values[index], static_cast<uint32_t>(a[2]), a.Ptr<uint8_t>(3), static_cast<uint32_t>(a[4]), a.Ptr<uint32_t>(5));
}

// NtSetValueKey(Key, PUNICODE_STRING Name, TitleIndex, Type, Data, DataSize)
NTSTATUS NtSetValueKey(Kernel& k, NtThread&, SyscallArgs& a) {
    auto key = k.Get<KeyObject>(a[0], ObjType::Key);
    if (!key) return STATUS_INVALID_HANDLE;
    Hive::Value v;
    v.name = Utf16ToUtf8(ReadUnicodeString(a.Ptr<UNICODE_STRING>(1)));
    v.type = static_cast<uint32_t>(a[3]);
    const auto* data = a.Ptr<uint8_t>(4);
    v.data.assign(data, data + static_cast<uint32_t>(a[5]));
    auto& ov = g_registry.OverlayFor(key->path);
    ov.deleted_values.erase(Lower(v.name));
    ov.values[Lower(v.name)] = std::move(v);
    return STATUS_SUCCESS;
}

NTSTATUS NtDeleteValueKey(Kernel& k, NtThread&, SyscallArgs& a) {
    auto key = k.Get<KeyObject>(a[0], ObjType::Key);
    if (!key) return STATUS_INVALID_HANDLE;
    const std::string name = Lower(Utf16ToUtf8(ReadUnicodeString(a.Ptr<UNICODE_STRING>(1))));
    auto& ov = g_registry.OverlayFor(key->path);
    ov.values.erase(name);
    ov.deleted_values.insert(name);
    return STATUS_SUCCESS;
}

NTSTATUS Succeed(Kernel&, NtThread&, SyscallArgs&) { return STATUS_SUCCESS; }

} // namespace

void InitRegistry(const std::string& config_dir) { g_registry.Init(config_dir); }

/// Registers the in-process activatable classes a package's manifest declares, the way package
/// deployment does: ActivatableClassId\<name> with the DLL that serves it.
void RegisterPackageClasses(const std::string& manifest_path) {
    std::ifstream in(manifest_path);
    if (!in) return;
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string xml = ss.str();
    auto utf16 = [](const std::string& text) {
        std::vector<uint8_t> out;
        for (char c : text) { out.push_back(static_cast<uint8_t>(c)); out.push_back(0); }
        out.push_back(0); out.push_back(0);
        return out;
    };
    auto dword = [](uint32_t v) { std::vector<uint8_t> out(4); std::memcpy(out.data(), &v, 4); return out; };
    size_t count = 0, at = 0;
    while ((at = xml.find("<InProcessServer>", at)) != std::string::npos) {
        const size_t end = xml.find("</InProcessServer>", at);
        if (end == std::string::npos) break;
        const std::string block = xml.substr(at, end - at);
        at = end;
        const size_t p0 = block.find("<Path>"), p1 = block.find("</Path>");
        if (p0 == std::string::npos || p1 == std::string::npos) continue;
        const std::string dll = block.substr(p0 + 6, p1 - p0 - 6);
        size_t c = 0;
        while ((c = block.find("ActivatableClassId=\"", c)) != std::string::npos) {
            c += 20;
            const size_t q = block.find('"', c);
            if (q == std::string::npos) break;
            const std::string id = block.substr(c, q - c);
            auto& ov = g_registry.OverlayFor("machine\\software\\microsoft\\windowsruntime\\activatableclassid\\" + Lower(id));
            ov.created = true;
            ov.display_name = id;
            {
                // combase resolves a class through a CLSID: ActivatableClassId\<id>\CLSID, then WindowsRuntime\CLSID\{clsid}.
                uint64_t h1 = 1469598103934665603ull, h2 = 0x9E3779B97F4A7C15ull;
                for (char ch : id) { h1 = (h1 ^ static_cast<uint8_t>(ch)) * 1099511628211ull; h2 = (h2 + static_cast<uint8_t>(ch)) * 0xBF58476D1CE4E5B9ull; }
                char guid[48];
                std::snprintf(guid, sizeof guid, "{%08x-%04x-%04x-%04x-%012llx}", static_cast<uint32_t>(h1 >> 32), static_cast<uint32_t>(h1 >> 16) & 0xFFFF,
                              0x4000 | (static_cast<uint32_t>(h2 >> 48) & 0x0FFF), 0x8000 | (static_cast<uint32_t>(h2 >> 32) & 0x3FFF),
                              static_cast<unsigned long long>(h2 & 0xFFFFFFFFFFFFull));
                ov.values["clsid"] = {"CLSID", 1, utf16(guid)};
                auto& cv = g_registry.OverlayFor("machine\\software\\microsoft\\windowsruntime\\clsid\\" + Lower(guid));
                cv.created = true;
                cv.display_name = guid;
                cv.values["activatableclassid"] = {"ActivatableClassId", 1, utf16(id)};
            }
            ov.values["dllpath"] = {"DllPath", 1, utf16("G:\\" + dll)};
            ov.values["activationtype"] = {"ActivationType", 4, dword(0)};
            ov.values["threading"] = {"Threading", 4, dword(0)};
            ov.values["trustlevel"] = {"TrustLevel", 4, dword(0)};
            ++count;
            c = q;
        }
    }
    UT_INFO("registry: %zu package classes from %s", count, manifest_path.c_str());
}

/// Development aid: prints every WinRT activatable class with the DLL that serves it.
void DumpActivatableClasses() {
    const auto root = g_registry.Resolve(Registry::Split("machine\\software\\microsoft\\windowsruntime\\activatableclassid"));
    for (const auto& name : g_registry.ChildNames(root)) {
        const auto node = g_registry.Resolve(Registry::Split("machine\\software\\microsoft\\windowsruntime\\activatableclassid\\" + name));
        std::string dll, type = "?";
        for (const auto& v : g_registry.ValuesOf(node)) {
            if (Lower(v.name) == "dllpath") dll = Utf16ToUtf8(std::u16string(reinterpret_cast<const char16_t*>(v.data.data()), v.data.size() / 2));
            if (Lower(v.name) == "activationtype" && v.data.size() >= 4) type = std::to_string(*reinterpret_cast<const uint32_t*>(v.data.data()));
        }
        std::printf("%s\t%s\t%s\n", name.c_str(), type.c_str(), dll.c_str());
    }
}

void RegisterRegistrySyscalls(std::unordered_map<std::string, SyscallFn>& t) {
    t["NtOpenKey"] = NtOpenKey;
    t["NtOpenKeyEx"] = NtOpenKey;
    t["NtCreateKey"] = NtCreateKey;
    t["NtQueryKey"] = NtQueryKey;
    t["NtEnumerateKey"] = NtEnumerateKey;
    t["NtQueryValueKey"] = NtQueryValueKey;
    t["NtEnumerateValueKey"] = NtEnumerateValueKey;
    t["NtSetValueKey"] = NtSetValueKey;
    t["NtDeleteValueKey"] = NtDeleteValueKey;
    t["NtDeleteKey"] = Succeed;
    t["NtFlushKey"] = Succeed;
    t["NtNotifyChangeKey"] = Succeed;
}

} // namespace utsuro::nt
