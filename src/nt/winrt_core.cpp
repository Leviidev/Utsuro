// winrt_core.cpp -- CoreApplication, its view, window and dispatcher: the application model
// a UWP title is built around. The title hands CoreApplication.Run a view source; the host
// creates the view, calls Initialize / SetWindow / Load / Run on it (in the guest, through the
// continuation mechanism) and raises Activated in between, which is what a real shell does.
#include "nt/winrt.h"

#include "engine/log.h"

namespace utsuro::nt::winrt {

namespace {

constexpr Out None = Out::None, Bool = Out::Bool, I32 = Out::I32, I64 = Out::I64, Ptr = Out::Ptr, GuidT = Out::Guid, Vec = Out::Vec, Map = Out::Map;

Method Get(const char* name, Out out) { return Method {name, out, 0, false, false, nullptr}; }
Method Put(const char* name) { return Method {name, Out::None, 0, false, false, nullptr}; }
Method Add(const char* name) { return Method {name, Out::None, 0, true, false, nullptr}; }
Method Remove(const char* name) { return Method {name, Out::None, 0, false, true, nullptr}; }
Method Fn(const char* name, Out out, HostFn fn) { return Method {name, out, 0, false, false, std::move(fn)}; }

uint64_t VtableSlot(uint64_t object, int slot) {
    const uint64_t vtable = *reinterpret_cast<uint64_t*>(object);
    return reinterpret_cast<uint64_t*>(vtable)[slot];
}

} // namespace

void RegisterCoreClasses(Runtime& rt) {
    // ---- the activation arguments ----------------------------------------------------
    Iface& activated_args = rt.DefineInterface("IActivatedEventArgs", "CF651713-CD08-4FD8-B697-A281B6544E2E", {
        Get("get_Kind", I32), Get("get_PreviousExecutionState", I32), Get("get_SplashScreen", Ptr),
    });

    Iface& launch_args = rt.DefineInterface("ILaunchActivatedEventArgs", "FBC93E26-A14A-4B4F-82B0-33BED920AF52", {
        Fn("get_Arguments", Ptr, [](Call& c) -> uint32_t { *c.Ptr<uint64_t>(0) = Runtime::Get().MakeHString(u""); return 0; }),
        Fn("get_TileId", Ptr, [](Call& c) -> uint32_t { *c.Ptr<uint64_t>(0) = Runtime::Get().MakeHString(u""); return 0; }),
    });

    // ---- the dispatcher --------------------------------------------------------------
    Iface& dispatcher = rt.DefineInterface("ICoreDispatcher", "60DB2FA8-B705-4FDE-A7D6-EBBB1891D39E", {
        Fn("get_HasThreadAccess", Bool, [](Call& c) -> uint32_t { *c.Ptr<uint8_t>(0) = 1; return 0; }),
        Fn("ProcessEvents", None, [&rt](Call& c) -> uint32_t {
            // Run everything queued with RunAsync, in order, then return.
            Obj& d = c.obj;
            if (d.queue.empty()) return 0;
            auto drain = std::make_shared<std::function<GuestStep(NtThread&, uint64_t)>>();
            *drain = [&rt, &d, drain](NtThread&, uint64_t) -> GuestStep {
                if (d.queue.empty()) return GuestStep::Return(0);
                const uint64_t handler = d.queue.front();
                d.queue.erase(d.queue.begin());
                return rt.InvokeDelegate(handler, {}, *drain);
            };
            c.thread.CallGuest((*drain)(c.thread, 0));
            return 0;
        }),
        Fn("RunAsync", Ptr, [](Call& c) -> uint32_t {
            c.obj.queue.push_back(c.Arg(1));
            if (auto* out = c.Ptr<uint64_t>(2)) *out = 0;
            return 0;
        }),
        Fn("RunIdleAsync", Ptr, [](Call& c) -> uint32_t {
            c.obj.queue.push_back(c.Arg(0));
            if (auto* out = c.Ptr<uint64_t>(1)) *out = 0;
            return 0;
        }),
    });

    // ---- the window ------------------------------------------------------------------
    static Obj* dispatcher_obj = nullptr;
    Iface& window = rt.DefineInterface("ICoreWindow", "79b9d5f2-879e-4b89-b798-79e47598030c", {
        Fn("get_Bounds", None, [](Call& c) -> uint32_t {
            auto* r = c.Ptr<float>(0);   // Rect { X, Y, Width, Height }
            r[0] = 0; r[1] = 0; r[2] = 1920; r[3] = 1080;
            return 0;
        }),
        Fn("get_Dispatcher", Ptr, [&dispatcher](Call&c ) -> uint32_t {
            auto& rt2 = Runtime::Get();
            if (dispatcher_obj == nullptr) dispatcher_obj = rt2.NewObject("Windows.UI.Core.CoreDispatcher", {&dispatcher});
            ++dispatcher_obj->refs;
            *c.Ptr<uint64_t>(0) = rt2.Pointer(*dispatcher_obj, dispatcher);
            return 0;
        }),
        Fn("get_IsInputEnabled", Bool, [](Call& c) -> uint32_t { *c.Ptr<uint8_t>(0) = 1; return 0; }),
        Put("put_IsInputEnabled"),
        Fn("get_Visible", Bool, [](Call& c) -> uint32_t { *c.Ptr<uint8_t>(0) = 1; return 0; }),
        Fn("Activate", None, [](Call&) -> uint32_t { return 0; }),
        Fn("Close", None, [](Call&) -> uint32_t { return 0; }),
        Get("GetAsyncKeyState", I32), Get("GetKeyState", I32),
        Add("add_Activated"), Remove("remove_Activated"),
        Add("add_CharacterReceived"), Remove("remove_CharacterReceived"),
        Add("add_Closed"), Remove("remove_Closed"),
        Add("add_InputEnabled"), Remove("remove_InputEnabled"),
        Add("add_KeyDown"), Remove("remove_KeyDown"),
        Add("add_KeyUp"), Remove("remove_KeyUp"),
        Add("add_PointerMoved"), Remove("remove_PointerMoved"),
        Add("add_PointerEntered"), Remove("remove_PointerEntered"),
        Add("add_PointerExited"), Remove("remove_PointerExited"),
        Add("add_SizeChanged"), Remove("remove_SizeChanged"),
        Add("add_VisibilityChanged"), Remove("remove_VisibilityChanged"),
    });
    static Obj* window_obj = nullptr;
    auto get_window = [&window]() -> Obj* {
        if (window_obj == nullptr) window_obj = Runtime::Get().NewObject("Windows.UI.Core.CoreWindow", {&window});
        return window_obj;
    };
    Iface& window_statics = rt.DefineInterface("ICoreWindowStatic", "4D239005-3C2A-41B1-9022-536BB9CF93B1", {
        Fn("GetForCurrentThread", Ptr, [&window, get_window](Call& c) -> uint32_t {
            Obj* w = get_window();
            ++w->refs;
            *c.Ptr<uint64_t>(0) = Runtime::Get().Pointer(*w, window);
            return 0;
        }),
    });
    rt.DefineClass("Windows.UI.Core.CoreWindow", [](Runtime&) -> Obj* { return nullptr; }, &window_statics);

    // ---- the application view --------------------------------------------------------
    static Obj* view_obj = nullptr;
    Iface& view = rt.DefineInterface("ICoreApplicationView", "638BB2DB-451D-4661-B099-414F34FFB9F1", {
        Fn("get_CoreWindow", Ptr, [&window, get_window](Call& c) -> uint32_t {
            Obj* w = get_window();
            ++w->refs;
            *c.Ptr<uint64_t>(0) = Runtime::Get().Pointer(*w, window);
            return 0;
        }),
        Add("add_Activated"), Remove("remove_Activated"),
        Fn("get_IsMain", Bool, [](Call& c) -> uint32_t { *c.Ptr<uint8_t>(0) = 1; return 0; }),
        Get("get_IsHosted", Bool),
    });
    auto get_view = [&view]() -> Obj* {
        if (view_obj == nullptr) view_obj = Runtime::Get().NewObject("Windows.ApplicationModel.Core.CoreApplicationView", {&view});
        return view_obj;
    };

    // ---- CoreApplication -------------------------------------------------------------
    Iface& app = rt.DefineInterface("ICoreApplication", "0aacf7a4-5e1d-49df-8034-fb6a68bc5ed1", {
        Fn("get_Id", Ptr, [&rt](Call& c) -> uint32_t { *c.Ptr<uint64_t>(0) = rt.MakeHString(u"App"); return 0; }),
        Add("add_Suspending"), Remove("remove_Suspending"),
        Add("add_Resuming"), Remove("remove_Resuming"),
        Get("get_Properties", Ptr),
        Fn("GetCurrentView", Ptr, [&view, get_view](Call& c) -> uint32_t {
            Obj* v = get_view();
            ++v->refs;
            *c.Ptr<uint64_t>(0) = Runtime::Get().Pointer(*v, view);
            return 0;
        }),
        Fn("Run", None, [&rt, &view, &window, &activated_args, &launch_args, get_view, get_window](Call& c) -> uint32_t {
            const uint64_t source = c.Arg(0);
            Obj* v = get_view();
            Obj* w = get_window();
            const uint64_t view_ptr = rt.Pointer(*v, view);
            const uint64_t window_ptr = rt.Pointer(*w, window);
            const uint64_t scratch = rt.AllocGuest(16);
            const uint64_t entry = rt.MakeHString(u"App");
            auto args_obj = rt.NewObject("Windows.ApplicationModel.Activation.LaunchActivatedEventArgs", {&activated_args, &launch_args});
            const uint64_t args_ptr = rt.Pointer(*args_obj, activated_args);
            UT_INFO("CoreApplication.Run: view source %#llx", (unsigned long long)source);
            // IFrameworkViewSource::CreateView -> IFrameworkView::{Initialize, SetWindow, Load}, Activated, Run, Uninitialize
            auto step_uninit = [](uint64_t fv) {
                return GuestStep::Call(VtableSlot(fv, 10), {fv}, [](NtThread&, uint64_t) { return GuestStep::Return(0); });
            };
            GuestStep first = GuestStep::Call(VtableSlot(source, 6), {source, scratch},
                [=, &rt](NtThread&, uint64_t hr) -> GuestStep {
                    const uint64_t fv = *reinterpret_cast<uint64_t*>(scratch);
                    UT_INFO("CreateView -> %#x, view %#llx", (uint32_t)hr, (unsigned long long)fv);
                    if (fv == 0) return GuestStep::Return(0x80004005u);
                    return GuestStep::Call(VtableSlot(fv, 6), {fv, view_ptr}, [=, &rt](NtThread&, uint64_t) -> GuestStep {
                        UT_INFO("Initialize done");
                        return GuestStep::Call(VtableSlot(fv, 7), {fv, window_ptr}, [=, &rt](NtThread&, uint64_t) -> GuestStep {
                            UT_INFO("SetWindow done");
                            return GuestStep::Call(VtableSlot(fv, 8), {fv, entry}, [=, &rt](NtThread& th, uint64_t) -> GuestStep {
                                UT_INFO("Load done; raising Activated");
                                // Activated handlers of the view get (sender, args).
                                auto handlers = std::make_shared<std::vector<uint64_t>>();
                                for (auto& [token, d] : v->delegates["Activated"]) handlers->push_back(d);
                                auto idx = std::make_shared<size_t>(0);
                                auto fire = std::make_shared<std::function<GuestStep(NtThread&, uint64_t)>>();
                                *fire = [=, &rt](NtThread&, uint64_t) -> GuestStep {
                                    if (*idx >= handlers->size()) {
                                        UT_INFO("running the view");
                                        return GuestStep::Call(VtableSlot(fv, 9), {fv}, [=](NtThread&, uint64_t) -> GuestStep {
                                            UT_INFO("view Run returned");
                                            return step_uninit(fv);
                                        });
                                    }
                                    const uint64_t h = (*handlers)[(*idx)++];
                                    return rt.InvokeDelegate(h, {view_ptr, args_ptr}, *fire);
                                };
                                return (*fire)(th, 0);
                            });
                        });
                    });
                });
            c.thread.CallGuest(first);
            return 0;
        }),
        Fn("RunWithActivationFactories", None, [](Call&) -> uint32_t { return 0x80004001u; }),
    });
    Iface& resource = rt.DefineInterface("ICoreApplicationResourceAvailability", "9FF3799E-6E83-468C-A5B0-501CE6BB0FA1", {
        Get("get_ResourceAvailability", I32), Add("add_ResourceAvailabilityChanged"), Remove("remove_ResourceAvailabilityChanged"),
    });
    Iface& gpu = rt.DefineInterface("ICoreApplicationGpuPolicy", "42A6A529-3CC6-4867-B5B7-6D1FAF5325F9", {
        Get("get_DisableKinectGpuReservation", Bool), Put("set_DisableKinectGpuReservation"),
    });
    Iface& immersive = rt.DefineInterface("ICoreImmersiveApplication", "1ADA0E3E-E4A2-4123-B451-DC96BF800419", {
        Fn("get_MainView", Ptr, [&view, get_view](Call& c) -> uint32_t {
            Obj* v = get_view();
            ++v->refs;
            *c.Ptr<uint64_t>(0) = Runtime::Get().Pointer(*v, view);
            return 0;
        }),
    });
    Iface& exit_if = rt.DefineInterface("ICoreApplicationExit", "CF86461D-261E-4B72-9ACD-44ED2ACE6A29", {
        Fn("Exit", None, [](Call& c) -> uint32_t { c.kernel.ExitProcess(0); return 0; }),
        Add("add_Exiting"), Remove("remove_Exiting"),
    });
    rt.DefineClassMulti("Windows.ApplicationModel.Core.CoreApplication", [](Runtime&) -> Obj* { return nullptr; },
                        {&app, &resource, &gpu, &immersive, &exit_if});
}

} // namespace utsuro::nt::winrt
