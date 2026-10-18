// winrt.h -- host-implemented WinRT classes. The guest sees ordinary COM objects whose
// vtable slots are tiny stubs that trap into the host; the host method sees `this` and the
// arguments and returns an HRESULT in rax.
#pragma once

#include "nt/kernel.h"

#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace utsuro::nt::winrt {

struct Guid {
    uint8_t b[16] {};
    static Guid Parse(const char* text);
    bool operator==(const Guid& o) const { return std::memcmp(b, o.b, 16) == 0; }
    std::string ToString() const;
};

/// What a default getter writes into its out parameter.
enum class Out { None, Bool, I32, I64, Ptr, Guid };

struct Obj;
struct Call;
using HostFn = std::function<uint32_t(Call&)>;  ///< Returns an HRESULT.

struct Method {
    std::string name;
    Out out {Out::None};   ///< The size of the value the default writes through the result pointer.
    int ret_arg {0};       ///< Which argument (after this) is that pointer: 0 for getters.
    bool event_add {false};
    bool event_remove {false};
    HostFn fn;             ///< Optional; without it the default behaviour applies.
};

struct Iface {
    std::string name;
    Guid iid;
    std::vector<Method> methods;       ///< From slot 6 on; slots 0-5 are IUnknown + IInspectable.
    uint64_t vtable {0};
    uint32_t first_id {0};
    bool guessed {false};              ///< The GUID is a placeholder; the first unknown IID asked for adopts it.
};

struct Obj {
    uint64_t id {0};
    std::string class_name;
    std::vector<Iface*> ifaces;
    std::vector<uint64_t> blocks;      ///< The guest-visible pointer for each interface.
    std::map<std::string, uint64_t> u64;
    std::map<std::string, std::string> str;
    /// Event handlers (guest COM delegates) by event name, each with its registration token.
    std::map<std::string, std::vector<std::pair<uint64_t, uint64_t>>> delegates;
    std::vector<uint64_t> queue;       ///< Pending work items (guest delegates) for a dispatcher.
    int refs {1};
};

/// One call into a host method.
struct Call {
    Kernel& kernel;
    NtThread& thread;
    SyscallArgs& args;
    Obj& obj;
    Iface& iface;
    uint32_t slot;                     ///< Method index within the interface (0-based after IInspectable).
    uint64_t Arg(int i) const { return args[i + 1]; }
    template <typename T>
    T* Ptr(int i) const { return reinterpret_cast<T*>(Arg(i)); }
};

class Runtime {
public:
    static Runtime& Get();
    void Init(Kernel& kernel);
    bool Ready() const { return ready_; }

    Iface& DefineInterface(const std::string& name, const char* iid, std::vector<Method> methods, bool guessed = false);
    /// Registers an activatable class: `statics` (may be null) is the factory's second interface.
    void DefineClass(const std::string& runtime_name, std::function<Obj*(Runtime&)> create, Iface* statics = nullptr);
    /// A class with several statics interfaces.
    void DefineClassMulti(const std::string& runtime_name, std::function<Obj*(Runtime&)> create, std::vector<Iface*> statics);

    Obj* NewObject(const std::string& class_name, std::vector<Iface*> ifaces);
    /// The guest pointer through which `obj` is seen as `iface`.
    uint64_t Pointer(Obj& obj, Iface& iface);

    Iface* FindInterface(const std::string& name);
    /// Replaces the body of the `nth` method called `method` of the interface called `iface`.
    bool Override(const std::string& iface, const std::string& method, HostFn fn, int nth = 0);
    /// RoGetActivationFactory for a registered class. Returns an HRESULT.
    uint32_t GetActivationFactory(const std::string& name, const Guid& iid, uint64_t* out);

    uint64_t MakeHString(const std::u16string& s);
    static std::u16string ReadHString(uint64_t h);
    uint64_t AllocGuest(size_t bytes, size_t align = 16);

    bool Dispatch(NtThread& thread, uint32_t number, SyscallArgs& args, uint64_t* result);

    static constexpr uint32_t kBase = 0xFFFE0000u;

private:
    Runtime() = default;
    void Materialize(Iface& iface);
    uint64_t QueryInterface(Obj& obj, const Guid& iid);

    Kernel* kernel_ {nullptr};
    bool ready_ {false};
    std::mutex mutex_;
    uint64_t heap_ {0}, heap_end_ {0};
    std::vector<std::unique_ptr<Iface>> ifaces_;
    std::vector<std::pair<Iface*, uint32_t>> method_table_;   // global id -> (iface, slot)
    std::map<uint64_t, std::unique_ptr<Obj>> objects_;
    uint64_t next_id_ {1};
    struct ClassDef { std::function<Obj*(Runtime&)> create; Iface* statics {nullptr}; std::vector<Iface*> more_statics; Obj* factory {nullptr}; };
    std::map<std::string, ClassDef> classes_;
    Iface* activation_factory_ {nullptr};
    uint64_t next_token_ {1};
    friend struct Call;
public:
    uint64_t NextToken() { return next_token_++; }
    /// Remembers `delegate` as a handler of `event` on `obj`; returns the registration token.
    uint64_t AddDelegate(Obj& obj, const std::string& event, uint64_t delegate);
    void RemoveDelegate(Obj& obj, const std::string& event, uint64_t token);
    /// Calls delegate->Invoke(args...) in the guest, then `then`. For use inside a host method.
    GuestStep InvokeDelegate(uint64_t delegate, std::initializer_list<uint64_t> args, std::function<GuestStep(NtThread&, uint64_t)> then = nullptr);
    Kernel& kernel() { return *kernel_; }
};

/// Registers every class the runtime provides (winrt_classes.cpp).
void RegisterClasses(Runtime& rt);
void RegisterCoreClasses(Runtime& rt);

} // namespace utsuro::nt::winrt
