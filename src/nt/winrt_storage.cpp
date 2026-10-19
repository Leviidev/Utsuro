// winrt_storage.cpp -- Windows.Xbox.Storage: connected storage, the console's save data. The
// host keeps each container as a directory of blobs below the save root.
#include "nt/winrt.h"

#include "engine/log.h"

#include <CommonCrypto/CommonDigest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace utsuro::nt::winrt {

namespace {

namespace fs = std::filesystem;
using Cont = std::function<GuestStep(uint64_t)>;   // continuation: gets the previous call's return value

Obj* g_space = nullptr;
/// The collections of one result struct type (ContainerInfo, ContainerInfo2, BlobInfo): the object
/// is an IVectorView and an IIterable of it, the iterator a separate object.
struct StructKind { Iface* view {nullptr}; Iface* iterable {nullptr}; Iface* iterator {nullptr}; };
StructKind g_kinds[3];

/// The IID of a parameterized WinRT interface: a name-based UUID of its signature.
Guid PInterfaceIid(const std::string& signature) {
    static const uint8_t ns[16] = {0x11, 0xf4, 0x7a, 0xd5, 0x7b, 0x73, 0x42, 0xc0, 0xab, 0xae, 0x87, 0x8b, 0x1e, 0x16, 0xad, 0xee};
    uint8_t d[CC_SHA1_DIGEST_LENGTH];
    CC_SHA1_CTX ctx;
    CC_SHA1_Init(&ctx);
    CC_SHA1_Update(&ctx, ns, sizeof ns);
    CC_SHA1_Update(&ctx, signature.data(), static_cast<CC_LONG>(signature.size()));
    CC_SHA1_Final(d, &ctx);
    d[6] = (d[6] & 0x0F) | 0x50;
    d[8] = (d[8] & 0x3F) | 0x80;
    Guid g;
    const int order[8] = {3, 2, 1, 0, 5, 4, 7, 6};
    for (int i = 0; i < 8; ++i) g.b[i] = d[order[i]];
    std::memcpy(g.b + 8, d + 8, 8);
    return g;
}

const char* kIIterable = "{faa585ea-6214-4217-afda-7f46de5869b3}";
const char* kKeyValuePair = "{02b51929-c1c4-4a7e-8940-0312b5c18500}";
const char* kIBuffer = "{905a0fe0-bc53-11df-8c49-001e4fc686da}";

const Guid& IterableOfString() {
    static const Guid g = PInterfaceIid(std::string("pinterface(") + kIIterable + ";string)");
    return g;
}
const Guid& IterableOfBlob() {
    static const Guid g = PInterfaceIid(std::string("pinterface(") + kIIterable + ";pinterface(" + kKeyValuePair + ";string;" + kIBuffer + "))");
    return g;
}
const Guid& ByteAccess() {
    static const Guid g = Guid::Parse("905a0fef-bc53-11df-8c49-001e4fc686da");
    return g;
}

// ---- The save directory: <root>/<container>/<blob>, names percent-escaped. ----

fs::path SaveRoot() {
    static const fs::path root = [] {
        const char* e = std::getenv("UTSURO_SAVES");
        fs::path p = e != nullptr ? fs::path(e) : fs::path(std::getenv("HOME")) / "Library/Application Support/Utsuro/saves";
        std::error_code ec;
        fs::create_directories(p, ec);
        return p;
    }();
    return root;
}

std::string Escape(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char ch : s) {
        if (std::isalnum(ch) || ch == '.' || ch == '-' || ch == '_') out += static_cast<char>(ch);
        else { out += '%'; out += hex[ch >> 4]; out += hex[ch & 15]; }
    }
    if (out.empty() || out[0] == '.') out.insert(0, "%2E");
    return out;
}

std::string Unescape(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() + 0 && std::isxdigit((unsigned char)s[i + 1]) && std::isxdigit((unsigned char)s[i + 2])) {
            out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else out += s[i];
    }
    return out;
}

fs::path ContainerDir(const std::string& container) { return SaveRoot() / Escape(container); }
fs::path BlobPath(const std::string& container, const std::string& blob) { return ContainerDir(container) / Escape(blob); }

bool ReadFile(const fs::path& p, std::vector<uint8_t>* out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

bool WriteFile(const fs::path& p, const uint8_t* data, size_t n) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = p.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(n));
        if (!f) return false;
    }
    fs::rename(tmp, p, ec);
    return !ec;
}

uint64_t DirectorySize(const fs::path& dir) {
    uint64_t total = 0;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file(ec)) total += e.file_size(ec);
    return total;
}

// ---- Calling into the guest's collections. ----

std::mutex g_scratch_mutex;
std::vector<uint64_t> g_scratch_free;

uint64_t TakeScratch() {
    std::scoped_lock lock {g_scratch_mutex};
    if (!g_scratch_free.empty()) { const uint64_t s = g_scratch_free.back(); g_scratch_free.pop_back(); std::memset(reinterpret_cast<void*>(s), 0, 256); return s; }
    return Runtime::Get().AllocGuest(256, 16);
}
void GiveScratch(uint64_t s) { std::scoped_lock lock {g_scratch_mutex}; g_scratch_free.push_back(s); }

uint64_t Slot(uint64_t object, int slot) {
    return reinterpret_cast<uint64_t*>(*reinterpret_cast<uint64_t*>(object))[slot];
}

GuestStep CallV(uint64_t object, int slot, std::initializer_list<uint64_t> extra, Cont k) {
    GuestStep s;
    s.call = true;
    s.fn = Slot(object, slot);
    s.args[0] = object;
    size_t i = 1;
    for (uint64_t v : extra) if (i < 4) s.args[i++] = v;
    if (k) s.cont = [k = std::move(k)](NtThread&, uint64_t r) { return k(r); };
    return s;
}

/// object->QueryInterface(iid); `k` gets the new interface pointer, or 0.
GuestStep Query(uint64_t object, const Guid& iid, uint64_t s, Cont k) {
    std::memcpy(reinterpret_cast<void*>(s), iid.b, 16);
    *reinterpret_cast<uint64_t*>(s + 16) = 0;
    return CallV(object, 0, {s, s + 16}, [s, k](uint64_t hr) {
        return k(static_cast<uint32_t>(hr) == 0 ? *reinterpret_cast<uint64_t*>(s + 16) : 0);
    });
}

GuestStep Release(uint64_t object, Cont k) { return CallV(object, 2, {}, std::move(k)); }

/// Walks an IIterable: First(), then HasCurrent / Current / MoveNext until it runs out.
struct Walk : std::enable_shared_from_this<Walk> {
    uint64_t it {0}, s {0};
    std::function<GuestStep(uint64_t, Cont)> body;
    Cont done;

    GuestStep Next() {
        auto me = shared_from_this();
        return CallV(it, 7, {s + 32}, [me](uint64_t) -> GuestStep {
            if (*reinterpret_cast<uint8_t*>(me->s + 32) == 0)
                return CallV(me->it, 2, {}, [me](uint64_t) { return me->done(0); });
            return CallV(me->it, 6, {me->s + 40}, [me](uint64_t) -> GuestStep {
                const uint64_t item = *reinterpret_cast<uint64_t*>(me->s + 40);
                return me->body(item, [me](uint64_t) {
                    return CallV(me->it, 8, {me->s + 32}, [me](uint64_t) { return me->Next(); });
                });
            });
        });
    }
};

GuestStep ForEach(uint64_t iterable, uint64_t s, std::function<GuestStep(uint64_t, Cont)> body, Cont done) {
    return CallV(iterable, 6, {s + 24}, [=](uint64_t hr) -> GuestStep {
        const uint64_t it = static_cast<uint32_t>(hr) == 0 ? *reinterpret_cast<uint64_t*>(s + 24) : 0;
        if (it == 0) return done(0);
        auto w = std::make_shared<Walk>();
        w->it = it; w->s = s; w->body = body; w->done = done;
        return w->Next();
    });
}

uint64_t SpacePointer() {
    Runtime& rt = Runtime::Get();
    if (g_space == nullptr) {
        g_space = rt.NewObject("Windows.Xbox.Storage.ConnectedStorageSpace",
                               {rt.FindInterface("IConnectedStorageSpace"), rt.FindInterface("IConnectedStorageSpace2")});
    }
    ++g_space->refs;
    return rt.Pointer(*g_space, *rt.FindInterface("IConnectedStorageSpace"));
}

int OutIndex(Call& c) { return c.iface.methods[c.slot].ret_arg; }

/// A host Windows.Storage.Streams.Buffer holding a copy of a blob.
uint64_t NewHostBuffer(const std::vector<uint8_t>& data) {
    Runtime& r = Runtime::Get();
    Obj* o = r.NewObject("Windows.Storage.Streams.Buffer", {r.FindInterface("IBuffer"), r.FindInterface("IBufferByteAccess")});
    const size_t cap = std::max<size_t>(data.size(), 1);
    auto* mem = static_cast<uint8_t*>(std::calloc(cap, 1));
    std::memcpy(mem, data.data(), data.size());
    o->u64["data"] = reinterpret_cast<uint64_t>(mem);
    o->u64["length"] = data.size();
    o->u64["capacity"] = cap;
    return r.Pointer(*o, *r.FindInterface("IBuffer"));
}

/// Writes the blob a guest IKeyValuePair<String, IBuffer> describes, then calls `next`.
GuestStep StoreBlob(const std::string& container, uint64_t kv, uint64_t s, Cont next) {
    return CallV(kv, 6, {s + 48}, [=](uint64_t) {
        return CallV(kv, 7, {s + 56}, [=](uint64_t) -> GuestStep {
            const std::string name = Utf16ToUtf8(Runtime::ReadHString(*reinterpret_cast<uint64_t*>(s + 48)));
            const uint64_t buffer = *reinterpret_cast<uint64_t*>(s + 56);
            auto finish = [=](uint64_t) { return Release(kv, next); };
            if (buffer == 0) return finish(0);
            return Query(buffer, ByteAccess(), s, [=](uint64_t access) -> GuestStep {
                if (access == 0) return Release(buffer, finish);
                return CallV(access, 3, {s + 72}, [=](uint64_t) {
                    return CallV(buffer, 7, {s + 80}, [=](uint64_t) {
                        const auto* data = reinterpret_cast<const uint8_t*>(*reinterpret_cast<uint64_t*>(s + 72));
                        const uint32_t length = *reinterpret_cast<uint32_t*>(s + 80);
                        const bool ok = data != nullptr && WriteFile(BlobPath(container, name), data, length);
                        UT_INFO("storage: write %s/%s (%u bytes)%s", container.c_str(), name.c_str(), length, ok ? "" : " FAILED");
                        return Release(access, [=](uint64_t) { return Release(buffer, finish); });
                    });
                });
            });
        });
    });
}

/// Fills the guest buffer of a guest IKeyValuePair<String, IBuffer> from the saved blob.
GuestStep LoadBlob(const std::string& container, uint64_t kv, uint64_t s, Cont next) {
    return CallV(kv, 6, {s + 48}, [=](uint64_t) {
        return CallV(kv, 7, {s + 56}, [=](uint64_t) -> GuestStep {
            const std::string name = Utf16ToUtf8(Runtime::ReadHString(*reinterpret_cast<uint64_t*>(s + 48)));
            const uint64_t buffer = *reinterpret_cast<uint64_t*>(s + 56);
            auto data = std::make_shared<std::vector<uint8_t>>();
            const bool found = ReadFile(BlobPath(container, name), data.get());
            UT_INFO("storage: read %s/%s -> %s (%zu bytes)", container.c_str(), name.c_str(), found ? "found" : "missing", data->size());
            auto finish = [=](uint64_t) { return Release(kv, next); };
            if (!found || buffer == 0) return buffer != 0 ? Release(buffer, finish) : finish(0);
            return Query(buffer, ByteAccess(), s, [=](uint64_t access) -> GuestStep {
                if (access == 0) return Release(buffer, finish);
                return CallV(access, 3, {s + 72}, [=](uint64_t) {
                    return CallV(buffer, 6, {s + 88}, [=](uint64_t) {
                        const uint32_t capacity = *reinterpret_cast<uint32_t*>(s + 88);
                        auto* dst = reinterpret_cast<uint8_t*>(*reinterpret_cast<uint64_t*>(s + 72));
                        const uint32_t n = static_cast<uint32_t>(std::min<size_t>(capacity, data->size()));
                        if (n < data->size()) UT_WARN("storage: buffer for %s holds %u of %zu bytes", name.c_str(), capacity, data->size());
                        if (dst != nullptr) std::memcpy(dst, data->data(), n);
                        return CallV(buffer, 8, {n}, [=](uint64_t) {
                            return Release(access, [=](uint64_t) { return Release(buffer, finish); });
                        });
                    });
                });
            });
        });
    });
}

/// Runs `body` for every item of the guest collection `source` (a pointer to any of its interfaces).
GuestStep EachOf(uint64_t source, const Guid& iterable_iid, uint64_t s, std::function<GuestStep(uint64_t, Cont)> body, Cont done) {
    if (source == 0) return done(0);
    return Query(source, iterable_iid, s, [=](uint64_t iterable) -> GuestStep {
        if (iterable == 0) return done(0);
        return ForEach(iterable, s, body, [=](uint64_t) { return Release(iterable, done); });
    });
}

} // namespace

void RegisterStorage(Runtime& rt) {
    auto fn = [](const char* name, HostFn f) { Method m; m.name = name; m.fn = std::move(f); return m; };
    // Windows.Storage.Streams.IBuffer / IBufferByteAccess over host memory.
    rt.DefineInterface("IBuffer", "905a0fe0-bc53-11df-8c49-001e4fc686da", {
        fn("get_Capacity", [](Call& c) -> uint32_t { *c.Ptr<uint32_t>(0) = static_cast<uint32_t>(c.obj.u64["capacity"]); return 0; }),
        fn("get_Length", [](Call& c) -> uint32_t { *c.Ptr<uint32_t>(0) = static_cast<uint32_t>(c.obj.u64["length"]); return 0; }),
        fn("put_Length", [](Call& c) -> uint32_t {
            if (c.Arg(0) > c.obj.u64["capacity"]) return 0x8007000Eu;   // E_OUTOFMEMORY stands in for E_BOUNDS
            c.obj.u64["length"] = c.Arg(0);
            return 0;
        }),
    });
    // Windows.Storage.Streams.Buffer is also activatable by titles (IBufferFactory::Create(capacity)).
    {
        Iface& factory = rt.DefineInterface("IBufferFactory", "71AF914D-C10F-484B-BC50-14BC623B3A27", {
            fn("Create", [](Call& c) -> uint32_t {
                const uint64_t capacity = c.Arg(0);
                const uint64_t buffer = NewHostBuffer(std::vector<uint8_t>(static_cast<size_t>(capacity)));
                if (Obj* o = Runtime::Get().FromPointer(buffer)) o->u64["length"] = 0;
                *c.Ptr<uint64_t>(1) = buffer;
                return 0;
            }),
        });
        rt.DefineClass("Windows.Storage.Streams.Buffer", [](Runtime&) -> Obj* { return nullptr; }, &factory);
    }
    rt.DefineComInterface("IBufferByteAccess", "905a0fef-bc53-11df-8c49-001e4fc686da", {
        [&] { Method m; m.name = "Buffer"; m.params = 1; m.fn = [](Call& c) -> uint32_t { *c.Ptr<uint64_t>(0) = c.obj.u64["data"]; return 0; }; return m; }(),
    }, [](Call&) -> uint32_t { return 0x80004001u; }).base_slots = 3;
    // IVectorView / IIterable over structs of `stride` 8-byte words (BlobInfo / ContainerInfo: 2,
    // ContainerInfo2: 5), flattened into items. Their IIDs are the computed ones for each struct type.
    auto struct_methods = [&](int kind) {
        std::vector<Method> view = {
            fn("GetAt", [](Call& c) -> uint32_t {
                const size_t w = c.obj.u64["stride"], i = c.Arg(0);
                if ((i + 1) * w > c.obj.items.size()) return 0x8000000Bu;
                std::memcpy(c.Ptr<void>(1), &c.obj.items[i * w], w * 8);
                return 0;
            }),
            fn("get_Size", [](Call& c) -> uint32_t {
                *c.Ptr<uint32_t>(0) = static_cast<uint32_t>(c.obj.items.size() / std::max<uint64_t>(c.obj.u64["stride"], 1));
                return 0;
            }),
            fn("IndexOf", [](Call& c) -> uint32_t { *c.Ptr<uint32_t>(1) = 0; *c.Ptr<uint8_t>(2) = 0; return 0; }),
            fn("GetMany", [](Call& c) -> uint32_t {
                const size_t w = c.obj.u64["stride"], start = c.Arg(0), cap = c.Arg(1);
                uint32_t n = 0;
                for (size_t i = start; (i + 1) * w <= c.obj.items.size() && n < cap; ++i, ++n)
                    std::memcpy(c.Ptr<uint64_t>(2) + n * w, &c.obj.items[i * w], w * 8);
                *c.Ptr<uint32_t>(3) = n;
                return 0;
            }),
        };
        return view;
    };
    struct KindIids { const char* iterable; const char* view; };
    const KindIids kind_iids[3] = {
        {"cd84dcca-b42a-5d4f-bfdc-324f2ec142bb", "8dbbed35-29ee-5c66-80c7-ab2e056ebf13"},   // ContainerInfo
        {"53f0bb98-9aef-5d69-9cf4-ba722af780de", "e8cc68f3-4c1f-5f92-b0b6-0a43c2471973"},   // ContainerInfo2
        {"14cc6c4b-2594-55a4-b0ae-781d253af09f", "51b469a2-6c7f-5109-8e13-ac209d44ac29"},   // BlobInfo
    };
    for (int k = 0; k < 3; ++k) {
        const std::string tag = std::to_string(k);
        g_kinds[k].view = &rt.DefineInterface("IStructView" + tag, kind_iids[k].view, struct_methods(k));
        g_kinds[k].iterator = &rt.DefineInterface("IStructIterator" + tag, "00000000-0000-0000-0000-000000000000", {
            fn("get_Current", [](Call& c) -> uint32_t {
                const size_t w = c.obj.u64["stride"], i = c.obj.u64["index"];
                if ((i + 1) * w > c.obj.items.size()) return 0x8000000Bu;
                std::memcpy(c.Ptr<void>(0), &c.obj.items[i * w], w * 8);
                return 0;
            }),
            fn("get_HasCurrent", [](Call& c) -> uint32_t {
                *c.Ptr<uint8_t>(0) = (c.obj.u64["index"] + 1) * c.obj.u64["stride"] <= c.obj.items.size();
                return 0;
            }),
            fn("MoveNext", [](Call& c) -> uint32_t {
                ++c.obj.u64["index"];
                *c.Ptr<uint8_t>(0) = (c.obj.u64["index"] + 1) * c.obj.u64["stride"] <= c.obj.items.size();
                return 0;
            }),
            fn("GetMany", [](Call& c) -> uint32_t {
                const size_t w = c.obj.u64["stride"], cap = c.Arg(0);
                uint32_t n = 0;
                for (size_t i = c.obj.u64["index"]; (i + 1) * w <= c.obj.items.size() && n < cap; ++i, ++n)
                    std::memcpy(c.Ptr<uint64_t>(1) + n * w, &c.obj.items[i * w], w * 8);
                c.obj.u64["index"] += n;
                *c.Ptr<uint32_t>(2) = n;
                return 0;
            }),
        }, true);
        g_kinds[k].iterable = &rt.DefineInterface("IStructIterable" + tag, kind_iids[k].iterable, {
            fn("First", [k](Call& c) -> uint32_t {
                Runtime& r = Runtime::Get();
                Obj* it = r.NewObject("Windows.Foundation.Collections.Iterator", {g_kinds[k].iterator});
                it->items = c.obj.items;
                it->u64["stride"] = c.obj.u64["stride"];
                *c.Ptr<uint64_t>(0) = r.Pointer(*it, *g_kinds[k].iterator);
                return 0;
            }),
        });
    }

    auto space_async = [](Call& c) -> uint32_t {
        *c.Ptr<uint64_t>(OutIndex(c)) = Runtime::Get().NewAsync(Out::Ptr, SpacePointer());
        return 0;
    };
    rt.OverrideAllNamed("IConnectedStorageSpaceStatics", "GetForUserAsync", space_async);
    rt.OverrideAllNamed("IConnectedStorageSpaceStatics", "GetForMachineAsync", space_async);
    rt.OverrideAllNamed("IConnectedStorageSpaceStatics2", "GetSyncOnDemandForUserAsync", space_async);

    rt.Override("IConnectedStorageSpace", "get_User", [](Call& c) -> uint32_t { *c.Ptr<uint64_t>(0) = CurrentUserPointer(); return 0; });
    rt.Override("IConnectedStorageSpace", "get_ServiceConfigurationId", [](Call& c) -> uint32_t {
        *c.Ptr<uint64_t>(0) = Runtime::Get().MakeHString(u"00000000-0000-0000-0000-000000000000");
        return 0;
    });
    rt.Override("IConnectedStorageSpace", "CreateContainer", [](Call& c) -> uint32_t {
        Runtime& r = Runtime::Get();
        Obj* o = r.NewObject("Windows.Xbox.Storage.ConnectedStorageContainer",
                             {r.FindInterface("IConnectedStorageContainer"), r.FindInterface("IConnectedStorageContainer2")});
        o->str["name"] = Utf16ToUtf8(Runtime::ReadHString(c.Arg(0)));
        *c.Ptr<uint64_t>(1) = r.Pointer(*o, *r.FindInterface("IConnectedStorageContainer"));
        UT_INFO("storage: container '%s'", o->str["name"].c_str());
        return 0;
    });
    rt.Override("IConnectedStorageSpace", "GetRemainingBytesInQuotaAsync", [](Call& c) -> uint32_t {
        *c.Ptr<uint64_t>(0) = Runtime::Get().NewAsync(Out::I32, 0x40000000);
        return 0;
    });
    rt.Override("IConnectedStorageSpace2", "GetRemainingBytesInQuota64Async", [](Call& c) -> uint32_t {
        *c.Ptr<uint64_t>(0) = Runtime::Get().NewAsync(Out::I64, 0x40000000ull);
        return 0;
    });

    rt.Override("IConnectedStorageContainer", "get_Name", [](Call& c) -> uint32_t {
        *c.Ptr<uint64_t>(0) = Runtime::Get().MakeHString(Utf8ToUtf16(c.obj.str["name"]));
        return 0;
    });
    rt.Override("IConnectedStorageContainer", "get_OwningSpace", [](Call& c) -> uint32_t { *c.Ptr<uint64_t>(0) = SpacePointer(); return 0; });
    // SubmitUpdatesAsync(blobs, deletes[, display name]): write the blobs, then delete the named ones.
    auto submit = [](Call& c) -> uint32_t {
        Runtime& r = Runtime::Get();
        const std::string container = c.obj.str["name"];
        const uint64_t blobs = c.Arg(0), deletes = c.Arg(1);
        if (OutIndex(c) >= 3 && c.Arg(2) != 0) {
            const std::string display = Utf16ToUtf8(Runtime::ReadHString(c.Arg(2)));
            WriteFile(ContainerDir(container) / ".display", reinterpret_cast<const uint8_t*>(display.data()), display.size());
        }
        *c.Ptr<uint64_t>(OutIndex(c)) = r.NewAsync(Out::None, 0);
        const uint64_t s = TakeScratch();
        Cont finish = [s](uint64_t) { GiveScratch(s); return GuestStep::Return(0); };
        Cont remove = [=](uint64_t) {
            return EachOf(deletes, IterableOfString(), s, [=](uint64_t name, Cont next) {
                const std::string n = Utf16ToUtf8(Runtime::ReadHString(name));
                std::error_code ec;
                fs::remove(BlobPath(container, n), ec);
                UT_INFO("storage: delete %s/%s", container.c_str(), n.c_str());
                return next(0);
            }, finish);
        };
        c.thread.CallGuest(EachOf(blobs, IterableOfBlob(), s, [=](uint64_t kv, Cont next) { return StoreBlob(container, kv, s, next); }, remove));
        return 0;
    };
    rt.Override("IConnectedStorageContainer", "SubmitUpdatesAsync", submit);
    rt.Override("IConnectedStorageContainer2", "SubmitUpdatesAsync", submit);
    // ReadAsync(blobs): fill the caller's buffers.
    rt.Override("IConnectedStorageContainer", "ReadAsync", [](Call& c) -> uint32_t {
        Runtime& r = Runtime::Get();
        const std::string container = c.obj.str["name"];
        const uint64_t blobs = c.Arg(0);
        *c.Ptr<uint64_t>(OutIndex(c)) = r.NewAsync(Out::None, 0);
        const uint64_t s = TakeScratch();
        c.thread.CallGuest(EachOf(blobs, IterableOfBlob(), s, [=](uint64_t kv, Cont next) { return LoadBlob(container, kv, s, next); },
                                  [s](uint64_t) { GiveScratch(s); return GuestStep::Return(0); }));
        return 0;
    });
    // GetAsync(names): a map of new buffers for the blobs that exist.
    rt.Override("IConnectedStorageContainer", "GetAsync", [](Call& c) -> uint32_t {
        const std::string container = c.obj.str["name"];
        uint64_t* out = c.Ptr<uint64_t>(OutIndex(c));
        const uint64_t s = TakeScratch();
        auto items = std::make_shared<std::vector<uint64_t>>();
        c.thread.CallGuest(EachOf(c.Arg(0), IterableOfString(), s, [=](uint64_t name, Cont next) {
            const std::string n = Utf16ToUtf8(Runtime::ReadHString(name));
            std::vector<uint8_t> data;
            const bool found = ReadFile(BlobPath(container, n), &data);
            UT_INFO("storage: get %s/%s -> %s (%zu bytes)", container.c_str(), n.c_str(), found ? "found" : "missing", data.size());
            if (found) {
                items->push_back(Runtime::Get().MakeHString(Utf8ToUtf16(n)));
                items->push_back(NewHostBuffer(data));
            }
            return next(0);
        }, [=](uint64_t) {
            Runtime& rr = Runtime::Get();
            *out = rr.NewAsync(Out::Ptr, rr.NewMapView(*items));
            GiveScratch(s);
            return GuestStep::Return(0);
        }));
        return 0;
    });
    rt.Override("IConnectedStorageContainer2", "CreateBlobInfoQuery", [](Call& c) -> uint32_t {
        Runtime& r = Runtime::Get();
        Obj* o = r.NewObject("Windows.Xbox.Storage.BlobInfoQueryResult", {r.FindInterface("IBlobInfoQueryResult")});
        o->str["container"] = c.obj.str["name"];
        o->str["prefix"] = Utf16ToUtf8(Runtime::ReadHString(c.Arg(0)));
        *c.Ptr<uint64_t>(1) = r.Pointer(*o, *r.FindInterface("IBlobInfoQueryResult"));
        return 0;
    });
    rt.Override("IConnectedStorageSpace", "DeleteContainerAsync", [](Call& c) -> uint32_t {
        const std::string name = Utf16ToUtf8(Runtime::ReadHString(c.Arg(0)));
        std::error_code ec;
        fs::remove_all(ContainerDir(name), ec);
        UT_INFO("storage: delete container %s", name.c_str());
        *c.Ptr<uint64_t>(OutIndex(c)) = Runtime::Get().NewAsync(Out::None, 0);
        return 0;
    });

    // Query results list the saved containers / blobs as arrays of structs.
    struct Entry { std::string name, display; uint64_t size; uint64_t filetime; };
    auto list = [](Call& c, bool blobs) {
        const std::string prefix = c.obj.str["prefix"];
        std::vector<Entry> found;
        std::error_code ec;
        const fs::path dir = blobs ? ContainerDir(c.obj.str["container"]) : SaveRoot();
        for (auto& e : fs::directory_iterator(dir, ec)) {
            const std::string file = e.path().filename().string();
            if (file[0] == '.' || (file.size() >= 4 && file.compare(file.size() - 4, 4, ".tmp") == 0)) continue;
            if (blobs ? !e.is_regular_file(ec) : !e.is_directory(ec)) continue;
            Entry entry;
            entry.name = Unescape(file);
            if (entry.name.compare(0, prefix.size(), prefix) != 0) continue;
            entry.size = blobs ? e.file_size(ec) : DirectorySize(e.path());
            entry.display = entry.name;
            std::vector<uint8_t> text;
            if (!blobs && ReadFile(e.path() / ".display", &text)) entry.display.assign(text.begin(), text.end());
            const auto mtime = fs::last_write_time(e.path(), ec).time_since_epoch();
            const int64_t unix_seconds = std::chrono::duration_cast<std::chrono::seconds>(mtime).count();
            entry.filetime = static_cast<uint64_t>(unix_seconds + 11644473600ll) * 10000000ull;
            found.push_back(std::move(entry));
        }
        std::sort(found.begin(), found.end(), [](const Entry& a, const Entry& b) { return a.name < b.name; });
        return found;
    };
    rt.Override("IConnectedStorageSpace", "CreateContainerInfoQuery", [=](Call& c) -> uint32_t {
        Runtime& r = Runtime::Get();
        Obj* o = r.NewObject("Windows.Xbox.Storage.ContainerInfoQueryResult",
                             {r.FindInterface("IContainerInfoQueryResult"), r.FindInterface("IContainerInfo2QueryResult")});
        o->str["prefix"] = Utf16ToUtf8(Runtime::ReadHString(c.Arg(0)));
        *c.Ptr<uint64_t>(1) = r.Pointer(*o, *r.FindInterface("IContainerInfoQueryResult"));
        return 0;
    });
    // kind 0: ContainerInfo, 1: ContainerInfo2, 2: BlobInfo.
    auto info_vector = [=](int kind) {
        return [=](Call& c) -> uint32_t {
            Runtime& r = Runtime::Get();
            const int out_index = OutIndex(c);
            const size_t start = out_index >= 2 ? c.Arg(0) : 0;
            const size_t count = out_index >= 2 ? c.Arg(1) : SIZE_MAX;
            const auto found = list(c, kind == 2);
            const StructKind& sk = g_kinds[kind];
            Obj* v = r.NewObject("Windows.Foundation.Collections.VectorView", {sk.view, sk.iterable});
            v->u64["stride"] = kind == 1 ? 5 : 2;
            for (size_t i = start; i < found.size() && i - start < count; ++i) {
                v->items.push_back(r.MakeHString(Utf8ToUtf16(found[i].name)));
                v->items.push_back(found[i].size);
                if (kind == 1) {
                    v->items.push_back(r.MakeHString(Utf8ToUtf16(found[i].display)));
                    v->items.push_back(found[i].filetime);
                    v->items.push_back(0);   // NeedsSync
                }
            }
            UT_INFO("storage: %s query '%s' -> %zu", kind == 2 ? "blob" : "container", c.obj.str["prefix"].c_str(), static_cast<size_t>(v->items.size() / v->u64["stride"]));
            *c.Ptr<uint64_t>(out_index) = r.NewAsync(Out::Ptr, r.Pointer(*v, *sk.view));
            return 0;
        };
    };
    auto item_count = [=](bool blobs) {
        return [=](Call& c) -> uint32_t {
            *c.Ptr<uint64_t>(OutIndex(c)) = Runtime::Get().NewAsync(Out::I32, list(c, blobs).size());
            return 0;
        };
    };
    rt.OverrideAllNamed("IContainerInfoQueryResult", "GetContainerInfoAsync", info_vector(0));
    rt.OverrideAllNamed("IContainerInfo2QueryResult", "GetContainerInfo2Async", info_vector(1));
    rt.OverrideAllNamed("IBlobInfoQueryResult", "GetBlobInfoAsync", info_vector(2));
    rt.OverrideAllNamed("IContainerInfoQueryResult", "GetItemCountAsync", item_count(false));
    rt.OverrideAllNamed("IBlobInfoQueryResult", "GetItemCountAsync", item_count(true));
}

} // namespace utsuro::nt::winrt
