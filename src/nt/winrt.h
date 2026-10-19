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
enum class Out { None, Bool, I32, I64, Ptr, Guid, Vec, Map };

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
    int params {0};        ///< Plain COM: parameter count after `this`.
    int out_index {-1};    ///< Plain COM: the parameter that returns a new object, if any.
    std::string out_type;  ///< Plain COM: that object's interface name.
    bool ctor {false};     ///< A factory CreateInstance: makes a new instance of the factory's class.
    bool async_op {false}; ///< Returns IAsyncOperation<T> / IAsyncAction through the trailing out parameter.
    Out async_kind {Out::None};   ///< The T of that operation.
};

struct Iface {
    std::string name;
    Guid iid;
    std::vector<Method> methods;       ///< From slot 6 on; slots 0-5 are IUnknown + IInspectable.
    uint64_t vtable {0};
    uint32_t first_id {0};
    bool guessed {false};              ///< The GUID is a placeholder; the first unknown IID asked for adopts it.
    int base_slots {6};                ///< 6 for WinRT (IUnknown + IInspectable), 3 for plain COM.
    HostFn fallback;                   ///< Runs for methods without their own `fn` (plain COM).
    uint32_t inline_table {0};         ///< Non-zero: the object embeds a copy of the vtable at this offset (Xbox device contexts).
    std::vector<Guid> aliases;         ///< Other IIDs answered with this interface's pointer (its bases).
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
    std::vector<uint64_t> items;       ///< Elements of a host collection.
    int refs {1};
    std::shared_ptr<void> user;        ///< Host state owned by whoever implements the object (the D3D layer's resources).
    bool ghost {false};                ///< Answers every IID it is asked for (a class the host does not model).
    bool recycle {false};              ///< Released to zero by the title means freed (transient objects, like D3D command lists).
    std::vector<size_t> block_sizes;   ///< Bytes of each block, to return them to the free list.
};

/// One call into a host method.
struct Call {
    Kernel& kernel;
    NtThread& thread;
    SyscallArgs& args;
    Obj& obj;
    Iface& iface;
    uint32_t slot;                     ///< Method index within the interface (0-based after IInspectable).
    uint64_t wide_result {0};          ///< A method returning a pointer sets this and wide_set instead of an HRESULT.
    bool wide_set {false};
    uint64_t Arg(int i) const { return args[i + 1]; }
    template <typename T>
    T* Ptr(int i) const { return reinterpret_cast<T*>(Arg(i)); }
    /// A float parameter after `this` (index i counts like Arg): the first four positions travel in XMM0-3.
    float Float(int i) const;
};

class Runtime {
public:
    static Runtime& Get();
    void Init(Kernel& kernel);
    bool Ready() const { return ready_; }

    Iface& DefineInterface(const std::string& name, const char* iid, std::vector<Method> methods, bool guessed = false);
    /// A plain COM interface (IUnknown only); slots are `methods` in order.
    Iface& DefineComInterface(const std::string& name, const char* iid, std::vector<Method> methods, HostFn fallback);
    /// Registers an activatable class: `statics` (may be null) is the factory's second interface.
    void DefineClass(const std::string& runtime_name, std::function<Obj*(Runtime&)> create, Iface* statics = nullptr);
    /// A class with several statics interfaces.
    void DefineClassMulti(const std::string& runtime_name, std::function<Obj*(Runtime&)> create, std::vector<Iface*> statics);

    /// `block_bytes`: the least size of an object's memory, for classes whose fields the title reads in place (D3D resources).
    Obj* NewObject(const std::string& class_name, std::vector<Iface*> ifaces, size_t block_bytes = 32);
    /// The guest pointer through which `obj` is seen as `iface`.
    uint64_t Pointer(Obj& obj, Iface& iface);

    Iface* FindInterface(const std::string& name);
    /// Replaces the body of the `nth` method called `method` of the interface called `iface`.
    bool Override(const std::string& iface, const std::string& method, HostFn fn, int nth = 0);
    /// RoGetActivationFactory for a registered class. Returns an HRESULT.
    uint32_t GetActivationFactory(const std::string& name, const Guid& iid, uint64_t* out);
    /// An already-completed IAsyncOperation<T> (or IAsyncAction when kind is None); returns its guest pointer.
    uint64_t NewAsync(Out kind, uint64_t value, uint32_t hr = 0);
    /// A read-only vector of interface pointers (IVectorView<T> for any T); returns the guest pointer.
    uint64_t NewVectorView(std::vector<uint64_t> items);
    /// A read-only String -> interface map; `key_value_pairs` is key HSTRING, value pointer, key, value...
    uint64_t NewMapView(std::vector<uint64_t> key_value_pairs);
    /// Replaces the body of every method called `method` in the interface called `iface`.
    int OverrideAllNamed(const std::string& iface, const std::string& method, HostFn fn);
    /// A stand-in object whose every interface succeeds without doing anything (logged).
    Obj* NewGhost(const std::string& class_name);

    uint64_t MakeHString(const std::u16string& s);
    static std::u16string ReadHString(uint64_t h);
    uint64_t AllocGuest(size_t bytes, size_t align = 16);

    bool Dispatch(NtThread& thread, uint32_t number, SyscallArgs& args, uint64_t* result);
    /// "Interface::method" for a host-method syscall number (debugging).
    std::string NameOf(uint32_t number) const;

    static constexpr uint32_t kBase = 0xFFFE0000u;

private:
    Runtime() = default;
    void Materialize(Iface& iface);
    uint64_t QueryInterface(Obj& obj, const Guid& iid);
    uint64_t AddGhostInterface(Obj& obj, const Guid& iid);

    Kernel* kernel_ {nullptr};
    bool ready_ {false};
    std::mutex mutex_;
    uint64_t heap_ {0}, heap_end_ {0}, heap_base_ {0};
    std::vector<std::unique_ptr<Iface>> ifaces_;
    std::vector<std::pair<Iface*, uint32_t>> method_table_;   // global id -> (iface, slot)
    std::map<uint64_t, std::unique_ptr<Obj>> objects_;
    std::map<size_t, std::vector<uint64_t>> free_blocks_;   // by size: the memory of freed objects
    std::atomic<uint64_t> next_id_ {1};   // objects are created on several threads
    struct ClassDef { std::function<Obj*(Runtime&)> create; Iface* statics {nullptr}; std::vector<Iface*> more_statics; Obj* factory {nullptr}; };
    std::map<std::string, ClassDef> classes_;
    Iface* activation_factory_ {nullptr};
    uint64_t next_token_ {1};
    friend struct Call;
public:
    /// The host object a guest interface pointer belongs to, or null.
    Obj* FromPointer(uint64_t pointer);
    /// Destroys `obj` (a `recycle` object the title released for the last time) and keeps its memory for the next one.
    void Free(Obj& obj);
    /// Called just before an object is destroyed.
    std::function<void(Obj&)> on_free;
    Iface* activation_factory_iface() { return activation_factory_; }
    uint64_t NextToken() { return next_token_++; }
    /// Remembers `delegate` as a handler of `event` on `obj`; returns the registration token.
    uint64_t AddDelegate(Obj& obj, const std::string& event, uint64_t delegate);
    void RemoveDelegate(Obj& obj, const std::string& event, uint64_t token);
    /// Calls delegate->Invoke(args...) in the guest, then `then`. For use inside a host method.
    GuestStep InvokeDelegate(uint64_t delegate, std::initializer_list<uint64_t> args, std::function<GuestStep(NtThread&, uint64_t)> then = nullptr);
    Kernel& kernel() { return *kernel_; }
    /// Called for every host method dispatched (diagnostics).
    std::function<void(Obj&, Iface&, const Method&)> observer;
};

/// Registers every class the runtime provides (winrt_classes.cpp).
void RegisterClasses(Runtime& rt);
uint64_t CurrentUserPointer();   ///< The one signed-in Windows.Xbox.System.User.
void RegisterCoreClasses(Runtime& rt);
void RegisterCollections(Runtime& rt);
void RegisterAsync(Runtime& rt);
void RegisterStorage(Runtime& rt);
void RegisterInput(Runtime& rt);
void RegisterAppData(Runtime& rt);

} // namespace utsuro::nt::winrt
