// winrt_async.cpp -- IAsyncOperation<T> / IAsyncAction as the host provides them: operations
// that have already finished. The title attaches a Completed handler and reads GetResults.
#include "nt/winrt.h"

#include "engine/log.h"

#include <cstring>

namespace utsuro::nt::winrt {

namespace {

Iface* g_async = nullptr;
Iface* g_info = nullptr;

constexpr uint32_t kStatusCompleted = 1, kStatusError = 3;

uint64_t VtableSlot(uint64_t object, int slot) {
    return reinterpret_cast<uint64_t*>(*reinterpret_cast<uint64_t*>(object))[slot];
}

} // namespace

void RegisterAsync(Runtime& rt) {
    auto fn = [](const char* name, HostFn f) { Method m; m.name = name; m.fn = std::move(f); return m; };
    // IAsyncOperation<T> / IAsyncAction: Completed handler, GetResults. The IID depends on T, so the
    // first IID the title asks for adopts this layout.
    g_async = &rt.DefineInterface("IAsyncOperation", "00000000-0000-0000-0000-000000000000", {
        fn("put_Completed", [](Call& c) -> uint32_t {
            const uint64_t handler = c.Arg(0);
            c.obj.u64["handler"] = handler;
            if (handler <= 0x10000) return 0;
            Runtime& r = Runtime::Get();
            const uint64_t self = r.Pointer(c.obj, c.iface);
            const uint32_t status = static_cast<uint32_t>(c.obj.u64["status"]);
            // The callee keeps its own reference to the handler, then (the operation being done) runs it.
            c.thread.CallGuest(GuestStep::Call(VtableSlot(handler, 1), {handler}, [&r, handler, self, status](NtThread&, uint64_t) {
                return r.InvokeDelegate(handler, {self, status}, [](NtThread&, uint64_t) { return GuestStep::Return(0); });
            }));
            return 0;
        }),
        fn("get_Completed", [](Call& c) -> uint32_t { *c.Ptr<uint64_t>(0) = c.obj.u64["handler"]; return 0; }),
        fn("GetResults", [](Call& c) -> uint32_t {
            const uint32_t hr = static_cast<uint32_t>(c.obj.u64["hr"]);
            if (hr != 0) return hr;
            void* out = c.Arg(0) != 0 ? c.Ptr<void>(0) : nullptr;
            if (out == nullptr) return 0;
            const uint64_t v = c.obj.u64["value"];
            switch (static_cast<Out>(c.obj.u64["kind"])) {
            case Out::Bool: *static_cast<uint8_t*>(out) = static_cast<uint8_t>(v); break;
            case Out::I32: *static_cast<uint32_t*>(out) = static_cast<uint32_t>(v); break;
            case Out::I64: case Out::Ptr: *static_cast<uint64_t*>(out) = v; break;
            case Out::Guid: if (v) std::memcpy(out, reinterpret_cast<void*>(v), 16); else std::memset(out, 0, 16); break;
            case Out::None: break;
            }
            return 0;
        }),
    }, true);
    g_info = &rt.DefineInterface("IAsyncInfo", "00000036-0000-0000-C000-000000000046", {
        fn("get_Id", [](Call& c) -> uint32_t { *c.Ptr<uint32_t>(0) = 1; return 0; }),
        fn("get_Status", [](Call& c) -> uint32_t { *c.Ptr<uint32_t>(0) = static_cast<uint32_t>(c.obj.u64["status"]); return 0; }),
        fn("get_ErrorCode", [](Call& c) -> uint32_t { *c.Ptr<uint32_t>(0) = static_cast<uint32_t>(c.obj.u64["hr"]); return 0; }),
        fn("Cancel", [](Call&) -> uint32_t { return 0; }),
        fn("Close", [](Call&) -> uint32_t { return 0; }),
    }, false);
}

uint64_t Runtime::NewAsync(Out kind, uint64_t value, uint32_t hr) {
    Obj* o = NewObject("Windows.Foundation.AsyncOperation", {g_async, g_info});
    o->u64["kind"] = static_cast<uint64_t>(kind);
    o->u64["value"] = value;
    o->u64["hr"] = hr;
    o->u64["status"] = hr == 0 ? kStatusCompleted : kStatusError;
    return Pointer(*o, *g_async);
}

} // namespace utsuro::nt::winrt
