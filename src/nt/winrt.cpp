#include "nt/winrt.h"

#include "engine/log.h"

#include <algorithm>
#include <cstdio>

namespace utsuro::nt::winrt {

namespace {
constexpr uint32_t kSOk = 0;
constexpr uint32_t kENoInterface = 0x80004002u;
constexpr uint32_t kEFail = 0x80004005u;
constexpr uint32_t kRegdbClassNotReg = 0x80040154u;

Guid kIUnknown = Guid::Parse("00000000-0000-0000-C000-000000000046");
Guid kIInspectable = Guid::Parse("AF86E2E0-B12D-4C6A-9C5A-D7AA65101E90");
Guid kIAgileObject = Guid::Parse("94EA2B94-E9CC-49E0-C0FF-EE64CA8F5B90");
} // namespace

Guid Guid::Parse(const char* text) {
    Guid g;
    unsigned a, b, c, d[8];
    char buf[64];
    std::snprintf(buf, sizeof buf, "%s", text);
    // Accept both {braced} and bare forms.
    const char* t = buf[0] == '{' ? buf + 1 : buf;
    if (std::sscanf(t, "%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x", &a, &b, &c, &d[0], &d[1], &d[2], &d[3], &d[4], &d[5], &d[6], &d[7]) != 11) return g;
    const uint32_t a32 = a;
    const uint16_t b16 = static_cast<uint16_t>(b), c16 = static_cast<uint16_t>(c);
    std::memcpy(g.b, &a32, 4);
    std::memcpy(g.b + 4, &b16, 2);
    std::memcpy(g.b + 6, &c16, 2);
    for (int i = 0; i < 8; ++i) g.b[8 + i] = static_cast<uint8_t>(d[i]);
    return g;
}

std::string Guid::ToString() const {
    char s[64];
    std::snprintf(s, sizeof s, "{%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}", *reinterpret_cast<const uint32_t*>(b),
                  *reinterpret_cast<const uint16_t*>(b + 4), *reinterpret_cast<const uint16_t*>(b + 6), b[8], b[9], b[10], b[11],
                  b[12], b[13], b[14], b[15]);
    return s;
}

Runtime& Runtime::Get() {
    static Runtime r;
    return r;
}

uint64_t Runtime::AllocGuest(size_t bytes, size_t align) {
    std::scoped_lock lock {mutex_};
    uint64_t at = (heap_ + align - 1) & ~(align - 1);
    if (at + bytes > heap_end_) {
        UT_ERROR("winrt: host heap exhausted");
        return 0;
    }
    heap_ = at + bytes;
    std::memset(reinterpret_cast<void*>(at), 0, bytes);
    return at;
}

void Runtime::Init(Kernel& kernel) {
    if (ready_) return;
    kernel_ = &kernel;
    uint64_t base = 0, size = 4 << 20;
    if (kernel.Vm().Allocate(&base, &size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE, RegionKind::Private, "host objects") != STATUS_SUCCESS) {
        UT_ERROR("winrt: cannot allocate the host heap");
        return;
    }
    heap_ = base;
    heap_end_ = base + size;
    ready_ = true;
    activation_factory_ = &DefineInterface("IActivationFactory", "00000035-0000-0000-C000-000000000046", {});
    activation_factory_->methods.push_back(Method {"ActivateInstance", Out::Ptr, 0, false, false, [this](Call& c) -> uint32_t {
        // The factory's own class is recorded on the object.
        auto it = classes_.find(c.obj.class_name);
        if (it == classes_.end()) return kEFail;
        Obj* instance = it->second.create(*this);
        if (instance == nullptr) return kEFail;
        *c.Ptr<uint64_t>(0) = Pointer(*instance, *instance->ifaces[0]);
        return kSOk;
    }});
    Materialize(*activation_factory_);
    RegisterClasses(*this);
    UT_INFO("winrt: %zu classes, %zu interfaces", classes_.size(), ifaces_.size());
}

void Runtime::Materialize(Iface& iface) {
    const size_t slots = 6 + iface.methods.size();
    iface.vtable = AllocGuest(slots * 8, 16);
    iface.first_id = static_cast<uint32_t>(method_table_.size());
    for (size_t s = 0; s < slots; ++s) {
        method_table_.emplace_back(&iface, static_cast<uint32_t>(s));
        const uint64_t stub = AllocGuest(16, 16);
        auto* p = reinterpret_cast<uint8_t*>(stub);
        p[0] = 0x4C; p[1] = 0x8B; p[2] = 0xD1;                 // mov r10, rcx
        p[3] = 0xB8;
        const uint32_t number = kBase + iface.first_id + static_cast<uint32_t>(s);
        std::memcpy(p + 4, &number, 4);                         // mov eax, number
        p[8] = 0x0F; p[9] = 0x05;                               // syscall
        p[10] = 0xC3;                                           // ret
        reinterpret_cast<uint64_t*>(iface.vtable)[s] = stub;
    }
}

Iface& Runtime::DefineInterface(const std::string& name, const char* iid, std::vector<Method> methods, bool guessed) {
    auto i = std::make_unique<Iface>();
    i->name = name;
    i->iid = Guid::Parse(iid);
    i->methods = std::move(methods);
    i->guessed = guessed;
    Iface& ref = *i;
    ifaces_.push_back(std::move(i));
    if (ready_ && !ref.methods.empty()) Materialize(ref);
    return ref;
}

Iface* Runtime::FindInterface(const std::string& name) {
    for (auto& i : ifaces_) if (i->name == name) return i.get();
    return nullptr;
}

bool Runtime::Override(const std::string& iface, const std::string& method, HostFn fn, int nth) {
    Iface* i = FindInterface(iface);
    if (i == nullptr) { UT_WARN("winrt: no interface %s to override", iface.c_str()); return false; }
    for (auto& m : i->methods) {
        if (m.name == method && nth-- == 0) { m.fn = std::move(fn); return true; }
    }
    UT_WARN("winrt: %s has no method %s", iface.c_str(), method.c_str());
    return false;
}

void Runtime::DefineClass(const std::string& runtime_name, std::function<Obj*(Runtime&)> create, Iface* statics) {
    classes_[runtime_name] = ClassDef {std::move(create), statics, nullptr};
}

Obj* Runtime::NewObject(const std::string& class_name, std::vector<Iface*> ifaces) {
    auto o = std::make_unique<Obj>();
    o->id = next_id_++;
    o->class_name = class_name;
    o->ifaces = std::move(ifaces);
    for (size_t i = 0; i < o->ifaces.size(); ++i) {
        Iface* f = o->ifaces[i];
        if (f->vtable == 0) Materialize(*f);
        const uint64_t block = AllocGuest(32, 16);
        reinterpret_cast<uint64_t*>(block)[0] = f->vtable;
        reinterpret_cast<uint64_t*>(block)[1] = o->id;
        reinterpret_cast<uint64_t*>(block)[2] = i;
        o->blocks.push_back(block);
    }
    Obj* raw = o.get();
    std::scoped_lock lock {mutex_};
    objects_[raw->id] = std::move(o);
    return raw;
}

uint64_t Runtime::Pointer(Obj& obj, Iface& iface) {
    for (size_t i = 0; i < obj.ifaces.size(); ++i)
        if (obj.ifaces[i] == &iface) return obj.blocks[i];
    return 0;
}

uint64_t Runtime::QueryInterface(Obj& obj, const Guid& iid) {
    if (iid == kIUnknown || iid == kIInspectable || iid == kIAgileObject) return obj.blocks.empty() ? 0 : obj.blocks[0];
    for (size_t i = 0; i < obj.ifaces.size(); ++i)
        if (obj.ifaces[i]->iid == iid) return obj.blocks[i];
    return 0;
}

uint32_t Runtime::GetActivationFactory(const std::string& name, const Guid& iid, uint64_t* out) {
    auto it = classes_.find(name);
    if (it == classes_.end()) return kRegdbClassNotReg;
    ClassDef& def = it->second;
    if (def.factory == nullptr) {
        std::vector<Iface*> ifaces {activation_factory_};
        if (def.statics != nullptr) ifaces.push_back(def.statics);
        def.factory = NewObject(name, ifaces);
    }
    uint64_t p = QueryInterface(*def.factory, iid);
    if (p == 0 && def.statics != nullptr) {
        UT_INFO("winrt: %s adopts %s for %s", def.statics->name.c_str(), iid.ToString().c_str(), name.c_str());
        def.statics->iid = iid;
        def.statics->guessed = false;
        p = QueryInterface(*def.factory, iid);
    }
    if (p == 0) {
        UT_WARN("winrt: factory of %s does not implement %s", name.c_str(), iid.ToString().c_str());
        *out = 0;
        return kENoInterface;
    }
    ++def.factory->refs;
    *out = p;
    return kSOk;
}

uint64_t Runtime::MakeHString(const std::u16string& s) {
    const uint64_t header = AllocGuest(0x20, 16);
    const uint64_t buf = AllocGuest((s.size() + 1) * 2, 16);
    std::memcpy(reinterpret_cast<void*>(buf), s.c_str(), (s.size() + 1) * 2);
    *reinterpret_cast<uint32_t*>(header) = 0x1;                       // a string reference
    *reinterpret_cast<uint32_t*>(header + 4) = static_cast<uint32_t>(s.size());
    *reinterpret_cast<uint64_t*>(header + 0x10) = buf;
    return header;
}

std::u16string Runtime::ReadHString(uint64_t h) {
    if (h == 0) return {};
    const uint32_t len = *reinterpret_cast<const uint32_t*>(h + 4);
    return std::u16string(reinterpret_cast<const char16_t*>(*reinterpret_cast<const uint64_t*>(h + 0x10)), len);
}

bool Runtime::Dispatch(NtThread& thread, uint32_t number, SyscallArgs& args, uint64_t* result) {
    if (!ready_ || number < kBase || number - kBase >= method_table_.size()) return false;
    auto [iface, slot] = method_table_[number - kBase];
    const uint64_t self = args[0];
    Obj* obj = nullptr;
    if (self > 0x10000) {
        const uint64_t id = reinterpret_cast<const uint64_t*>(self)[1];
        std::scoped_lock lock {mutex_};
        auto it = objects_.find(id);
        if (it != objects_.end()) obj = it->second.get();
    }
    if (obj == nullptr) {
        UT_ERROR("winrt: call of %s slot %u on a bad object %#llx", iface->name.c_str(), slot, (unsigned long long)self);
        *result = kEFail;
        return true;
    }
    switch (slot) {
    case 0: {  // QueryInterface(riid, ppv)
        const Guid iid = *reinterpret_cast<const Guid*>(args[1]);
        uint64_t p = QueryInterface(*obj, iid);
        if (p == 0) {
            for (Iface* f : obj->ifaces) {
                if (f->guessed && f != activation_factory_) {
                    UT_INFO("winrt: %s adopts %s for %s", f->name.c_str(), iid.ToString().c_str(), obj->class_name.c_str());
                    f->iid = iid;
                    f->guessed = false;
                    p = QueryInterface(*obj, iid);
                    break;
                }
            }
        }
        if (p == 0) {
            UT_INFO("winrt: %s does not implement %s", obj->class_name.c_str(), iid.ToString().c_str());
            *reinterpret_cast<uint64_t*>(args[2]) = 0;
            *result = kENoInterface;
        } else {
            ++obj->refs;
            *reinterpret_cast<uint64_t*>(args[2]) = p;
            *result = kSOk;
        }
        return true;
    }
    case 1: *result = static_cast<uint32_t>(++obj->refs); return true;
    case 2: *result = static_cast<uint32_t>(obj->refs > 1 ? --obj->refs : 1); return true;
    case 3: {  // GetIids(ULONG*, IID**)
        *reinterpret_cast<uint32_t*>(args[1]) = 0;
        *reinterpret_cast<uint64_t*>(args[2]) = 0;
        *result = kSOk;
        return true;
    }
    case 4: {  // GetRuntimeClassName(HSTRING*)
        *reinterpret_cast<uint64_t*>(args[1]) = MakeHString(Utf8ToUtf16(obj->class_name));
        *result = kSOk;
        return true;
    }
    case 5: *reinterpret_cast<uint32_t*>(args[1]) = 0; *result = kSOk; return true;  // GetTrustLevel
    default: break;
    }
    Method& m = iface->methods[slot - 6];
    Call call {*kernel_, thread, args, *obj, *iface, slot - 6};
    UT_DEBUG("winrt: %s.%s::%s", obj->class_name.c_str(), iface->name.c_str(), m.name.c_str());
    if (m.fn) { *result = m.fn(call); return true; }
    if (m.event_add) {
        if (auto* tok = call.Ptr<uint64_t>(1)) *tok = NextToken();
        *result = kSOk;
        return true;
    }
    if (m.event_remove) { *result = kSOk; return true; }
    if (m.out != Out::None) {
        void* out = call.Ptr<void>(m.ret_arg);
        if (out != nullptr) {
            const size_t n = m.out == Out::Bool ? 1 : m.out == Out::I32 ? 4 : m.out == Out::Guid ? 16 : 8;
            std::memset(out, 0, n);
        }
    }
    *result = kSOk;
    return true;
}

} // namespace utsuro::nt::winrt
