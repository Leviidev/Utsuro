// winrt_classes.cpp -- the Windows.Xbox.* classes the titles activate. Interface and method
// order follow WinDurango's IDL notes; a method without a body uses the generic default
// (getters write zero, setters succeed, events hand out tokens).
#include "nt/winrt.h"
#include "nt/safe_mem.h"
#include "nt/kernel.h"
#include <cstdio>
#include <cstdlib>
#include <string>

#include "engine/log.h"

namespace utsuro::nt::winrt {

namespace {

constexpr Out None = Out::None, Bool = Out::Bool, I32 = Out::I32, I64 = Out::I64, Ptr = Out::Ptr, GuidT = Out::Guid, Vec = Out::Vec, Map = Out::Map;

Method Get(const char* name, Out out) { return Method {name, out, 0, false, false, nullptr}; }
Method Put(const char* name) { return Method {name, Out::None, 0, false, false, nullptr}; }
Method Add(const char* name) { return Method {name, Out::None, 0, true, false, nullptr}; }
Method Remove(const char* name) { return Method {name, Out::None, 0, false, true, nullptr}; }
Method Result(const char* name, Out out, int in_params) { return Method {name, out, in_params, false, false, nullptr}; }
Method Ctor(const char* name, int in_params) {
    Method m;
    m.name = name;
    m.ret_arg = in_params;
    m.ctor = true;
    return m;
}
Method AsyncResult(const char* name, Out kind, int in_params) {
    Method m;
    m.name = name;
    m.ret_arg = in_params;
    m.async_op = true;
    m.async_kind = kind;
    return m;
}
Method Fn(const char* name, Out out, HostFn fn) { return Method {name, out, 0, false, false, std::move(fn)}; }

#include "nt/winrt_generated.inc"

} // namespace

static std::vector<std::pair<uint64_t, uint64_t>> g_pending_user_added;   // (handler, sender)

static int g_pumps_since_subscribe = 0;

/// UserAdded is raised a little after the title subscribes (about 1.5 s of frames), as the console does, not at its first pump.
bool HasPendingUserAdded() {
    if (g_pending_user_added.empty()) return false;
    return ++g_pumps_since_subscribe > 90;
}

GuestStep RaisePendingUserAdded(std::function<GuestStep(NtThread&, uint64_t)> then) {
    if (g_pending_user_added.empty()) return GuestStep::Call(0, {}, nullptr);   // not reached: callers check HasPendingUserAdded
    Runtime& r = Runtime::Get();
    const auto [handler, sender] = g_pending_user_added.front();
    UT_INFO("winrt: raising UserAdded to %#llx", (unsigned long long)handler);
    if (std::getenv("UTSURO_DELEGATELOG") != nullptr) {
        const uint64_t* vt = *reinterpret_cast<const uint64_t* const*>(handler);
        UT_INFO("winrt:   delegate vtable %p: %#llx %#llx %#llx %#llx; [+8] %#llx [+0x128] %#llx", (const void*)vt, (unsigned long long)vt[0], (unsigned long long)vt[1], (unsigned long long)vt[2], (unsigned long long)vt[3],
                (unsigned long long)*reinterpret_cast<const uint64_t*>(handler + 8), (unsigned long long)*reinterpret_cast<const uint64_t*>(handler + 0x128));
    }
    g_pending_user_added.erase(g_pending_user_added.begin());
    Iface* args_iface = r.FindInterface("IUserAddedEventArgs");
    Obj* args = r.NewObject("Windows.Xbox.System.UserAddedEventArgs", {args_iface});
    const uint64_t args_ptr = r.Pointer(*args, *args_iface);
    return r.InvokeDelegate(handler, {sender, args_ptr}, [then](NtThread& t, uint64_t) -> GuestStep {
        if (HasPendingUserAdded()) return RaisePendingUserAdded(then);
        return then(t, 0);
    });
}

uint64_t CurrentUserPointer() {
    static Obj* user = nullptr;
    Runtime& r = Runtime::Get();
    Iface* user_if = r.FindInterface("IUser");
    if (user == nullptr) user = r.NewObject("Windows.Xbox.System.User", {user_if});
    ++user->refs;
    return r.Pointer(*user, *user_if);
}

void RegisterClasses(Runtime& rt) {
    RegisterGenerated(rt);
    RegisterCoreClasses(rt);
    RegisterCollections(rt);
    RegisterAsync(rt);
    RegisterStorage(rt);
    RegisterAppData(rt);
    // Windows.Xbox.Achievements.AchievementNotifier
    {
        Iface* notifier_if = &rt.DefineInterface("IAchievementNotifier", "00000000-0000-0000-0000-000000000000", {});
        notifier_if->methods.push_back(Method {"add_AchievementEarned", Out::Ptr, 1, false, false, [](Call& c) -> uint32_t {
            *c.Ptr<uint64_t>(1) = 1;
            return 0;
        }});
        Iface* statics_if = &rt.DefineInterface("IAchievementNotifierStatics", "82ca180b-30e2-4ea0-a62e-b61697a2f334", {});
        statics_if->methods.push_back(Method {"get_Notifier", Out::Ptr, 0, false, false, [notifier_if](Call& c) -> uint32_t {
            Runtime& r = Runtime::Get();
            static Obj* notifier = nullptr;
            if (notifier == nullptr) notifier = r.NewObject("Windows.Xbox.Achievements.AchievementNotifier", {notifier_if});
            ++notifier->refs;
            *c.Ptr<uint64_t>(0) = r.Pointer(*notifier, *notifier_if);
            return 0;
        }});
        rt.DefineClass("Windows.Xbox.Achievements.AchievementNotifier", [notifier_if](Runtime& r) -> Obj* {
            return r.NewObject("Windows.Xbox.Achievements.AchievementNotifier", {notifier_if});
        }, statics_if);
    }
    // Windows.Kinect.KinectSensor: a console without a Kinect still hands out a sensor object (IsAvailable false);
    // titles deref it (Forza Horizon 2 uses it for voice). Its methods are unknown by slot: they log, bool/pointer
    // results stay zero, and they succeed.
    {
        Iface* sensor_if = &rt.DefineInterface("ghost:IKinectSensor", "00000000-0000-0000-0000-000000000000", {});
        for (int i = 0; i < 64; ++i) { Method m; m.name = "slot" + std::to_string(6 + i); sensor_if->methods.push_back(std::move(m)); }
        sensor_if->fallback = [sensor_if](Call& c) -> uint32_t {
            // Getter-shaped calls (one zeroed 8-byte local in the caller's frame as the result) get a nested ghost
            // object, so the title can keep walking the sensor's sub-objects; IsOpen/IsAvailable (slots 10/11 in the
            // Kinect v2 layout: events, Open, Close, then the bools) stay false; everything else just succeeds.
            const uint32_t slot = c.slot + 6;
            int out = -1;
            for (int i = 0; i < 4; ++i) {
                const uint64_t a = c.Arg(i);
                uint64_t v = 0;
                if (a >= c.args.rsp && a < c.args.rsp + 0x10000 && (a & 7) == 0 && SafeRead(a, &v, 8) && v == 0) { out = i; break; }
            }
            std::string what = "S_OK";
            if (out >= 0 && c.obj.class_name == "Windows.Kinect.KinectSensor" && (slot == 10 || slot == 11)) {
                *c.Ptr<uint8_t>(out) = 0; what = "false";
            } else if (out >= 0 && out == 0) {
                Runtime& r = Runtime::Get();
                Obj* sub = r.NewObject(c.obj.class_name + "#" + std::to_string(slot), {sensor_if});
                sub->ghost = true;
                ++sub->refs;
                *c.Ptr<uint64_t>(out) = r.Pointer(*sub, *sensor_if);
                what = "ghost object";
            }
            static std::atomic<int> shown {0};
            if (shown++ < 60) UT_INFO("kinect: %s slot %u (%#llx, %#llx, %#llx) -> %s", c.obj.class_name.c_str(), slot, (unsigned long long)c.Arg(0), (unsigned long long)c.Arg(1), (unsigned long long)c.Arg(2), what.c_str());
            return 0;
        };
        Iface* statics_if = &rt.DefineInterface("IKinectSensorStatics", "00000000-0000-0000-0000-000000000001", {});
        statics_if->guessed = true;
        statics_if->methods.push_back(Method {"GetDefault", Out::Ptr, 0, false, false, [sensor_if](Call& c) -> uint32_t {
            Runtime& r = Runtime::Get();
            static Obj* sensor = nullptr;
            if (sensor == nullptr) { sensor = r.NewObject("Windows.Kinect.KinectSensor", {sensor_if}); sensor->ghost = true; }
            ++sensor->refs;
            *c.Ptr<uint64_t>(0) = r.Pointer(*sensor, *sensor_if);
            return 0;
        }});
        rt.DefineClass("Windows.Kinect.KinectSensor", [](Runtime&) -> Obj* { return nullptr; }, statics_if);
    }
    // Microsoft.Xbox.Services.XboxLiveContext: there is no Xbox Live here. The context's properties (all of them are
    // service getters, in an order that depends on the XSAPI build the title shipped) hand out "service" objects whose
    // every method fails with E_FAIL, the way a service call fails on a console without a connection; titles handle
    // that. The title calls the context's methods on the pointer the factory returned without a QueryInterface, so that
    // pointer's vtable must be the context's own.
    {
        Iface* factory_if = &rt.DefineInterface("IXboxLiveContextFactory", "bedc781e-a29e-3333-9d97-7af19812192d", {});
        factory_if->methods.push_back(Method {"CreateXboxLiveContext", Out::Ptr, 1, false, false, [](Call& c) -> uint32_t {
            Runtime& r = Runtime::Get();
            static Iface* context_if = nullptr;
            static Iface* service_if = nullptr;
            if (context_if == nullptr) {
                service_if = &r.DefineInterface("ghost:XboxLiveService", "00000000-0000-0000-0000-000000000000", {});
                for (int i = 0; i < 64; ++i) { Method m; m.name = "slot" + std::to_string(6 + i); service_if->methods.push_back(std::move(m)); }
                service_if->fallback = [](Call& c) -> uint32_t {
                    // Service methods return an async operation through their last parameter, a pointer into the
                    // caller's stack frame; the arguments before it are strings and objects on the heap. The operation
                    // is handed back already failed, which is what a service call comes to with no connection.
                    std::string shown;
                    int out = -1;
                    for (int i = 0; i < 11; ++i) {
                        const uint64_t a = c.Arg(i);
                        char t[64];
                        std::snprintf(t, sizeof t, "%s%#llx", i ? ", " : "", (unsigned long long)a);
                        shown += t;
                        // The result pointer: a zeroed local in the caller's frame (strings passed by reference sit
                        // on the stack too, but hold their header, not zero).
                        uint64_t head[3] = {0, 0, 0};   // anything can sit in the stack slots past the real arguments
                        if (a >= c.args.rsp && a < c.args.rsp + 0x10000 && (a & 7) == 0 && SafeRead(a, head, 8) && head[0] == 0) out = i;
                        else if (a > 0x10000 && (a & 3) == 0 && SafeRead(a, head, 24) && (head[0] & 0xFFFFFFFF) == 1 && (head[0] >> 32) < 256 && head[2] > 0x10000)
                            shown += " \"" + Utf16ToUtf8(Runtime::ReadHString(a)) + "\"";
                    }
                    Runtime& rr = Runtime::Get();
                    if (out >= 0) *c.Ptr<uint64_t>(out) = rr.NewAsync(Out::Ptr, 0, 0x80004005u);
                    UT_INFO("xsapi: %s slot %u (%s) -> %s", c.obj.class_name.c_str(), c.slot + 6, shown.c_str(), out >= 0 ? "failed async operation" : "S_OK (no result pointer)");
                    GuestCpuState st;
                    if (c.thread.control != nullptr && c.thread.control->CaptureCpuState(&st)) {
                        uint64_t regs[16];
                        for (int i = 0; i < 16; ++i) regs[i] = st.gregs[i];
                        const auto frames = c.kernel.Backtrace(st.rip, regs, 8);
                        for (size_t i = 0; i < frames.size(); ++i) UT_INFO("    #%zu %s", i, c.kernel.Describe(frames[i]).c_str());
                    }
                    return 0;   // methods without a result (Activate, subscriptions) succeed quietly
                };
                context_if = &r.DefineInterface("ghost:IXboxLiveContext", "00000000-0000-0000-0000-000000000000", {});
                for (int i = 0; i < 48; ++i) { Method m; m.name = "slot" + std::to_string(6 + i); context_if->methods.push_back(std::move(m)); }
                context_if->fallback = [](Call& c) -> uint32_t {
                    Runtime& rr = Runtime::Get();
                    if (c.slot == 0) {   // get_User comes first in every XSAPI build
                        *c.Ptr<uint64_t>(0) = CurrentUserPointer();
                        return 0;
                    }
                    const std::string key = "service" + std::to_string(c.slot);
                    if (c.obj.u64[key] == 0) {
                        Obj* svc = rr.NewObject("Microsoft.Xbox.Services.Service#" + std::to_string(c.slot + 6), {service_if});
                        ++svc->refs;
                        c.obj.u64[key] = rr.Pointer(*svc, *service_if);
                    }
                    UT_INFO("xsapi: XboxLiveContext slot %u -> service object", c.slot + 6);
                    *c.Ptr<uint64_t>(0) = c.obj.u64[key];
                    return 0;
                };
            }
            Obj* ctx = r.NewObject("Microsoft.Xbox.Services.XboxLiveContext", {context_if});
            ctx->ghost = true;
            ++ctx->refs;
            *c.Ptr<uint64_t>(1) = r.Pointer(*ctx, *context_if);
            return 0;
        }});
        rt.DefineClass("Microsoft.Xbox.Services.XboxLiveContext", [](Runtime& r) -> Obj* {
            return r.NewGhost("Microsoft.Xbox.Services.XboxLiveContext");
        }, factory_if);
    }
    // One signed-in user, id 1, who plays on the one pad.
    {
        // GetTokenAndSignatureAsync: a result whose strings are all empty (there is no Xbox Live to sign); titles
        // deref the result before looking at it.
        rt.OverrideAllNamed("IUser", "GetTokenAndSignatureAsync", [](Call& c) -> uint32_t {
            Runtime& r = Runtime::Get();
            Iface* result_if = r.FindInterface("IGetTokenAndSignatureResult");
            Obj* res = r.NewObject("Windows.Xbox.System.GetTokenAndSignatureResult", {result_if});
            ++res->refs;
            *c.Ptr<uint64_t>(c.iface.methods[c.slot].ret_arg) = r.NewAsync(Out::Ptr, r.Pointer(*res, *result_if));
            return 0;
        });
        rt.Override("IUserStatics", "get_Users", [](Call& c) -> uint32_t {
            *c.Ptr<uint64_t>(0) = Runtime::Get().NewVectorView({CurrentUserPointer()});
            return 0;
        });
        rt.Override("IUserStatics", "GetUserById", [](Call& c) -> uint32_t {
            *c.Ptr<uint64_t>(1) = c.Arg(0) == 1 ? CurrentUserPointer() : 0;
            return 0;
        });
        rt.Override("IUser", "get_Id", [](Call& c) -> uint32_t { *c.Ptr<uint32_t>(0) = 1; return 0; });
        rt.Override("IUser", "get_IsSignedIn", [](Call& c) -> uint32_t { *c.Ptr<uint8_t>(0) = 1; return 0; });
        rt.Override("IUser", "get_XboxUserId", [](Call& c) -> uint32_t { *c.Ptr<uint64_t>(0) = Runtime::Get().MakeHString(u"2535400000000001"); return 0; });
        rt.Override("IUser", "get_DisplayInfo", [](Call& c) -> uint32_t {
            static Obj* info = nullptr;
            Runtime& r = Runtime::Get();
            Iface* i = r.FindInterface("IUserDisplayInfo");
            if (info == nullptr) info = r.NewObject("Windows.Xbox.System.UserDisplayInfo", {i});
            ++info->refs;
            *c.Ptr<uint64_t>(0) = r.Pointer(*info, *i);
            return 0;
        });
        for (const char* name : {"Gamertag", "ApplicationDisplayName", "GameDisplayName"})
            rt.Override("IUserDisplayInfo", name, [](Call& c) -> uint32_t { *c.Ptr<uint64_t>(0) = Runtime::Get().MakeHString(u"Steve"); return 0; });
        rt.Override("IUserDisplayInfo", "GamerScore", [](Call& c) -> uint32_t { *c.Ptr<uint32_t>(0) = 0; return 0; });
        rt.Override("IUserDisplayInfo", "Reputation", [](Call& c) -> uint32_t { *c.Ptr<uint32_t>(0) = 50; return 0; });
        rt.Override("IUserDisplayInfo", "AgeGroup", [](Call& c) -> uint32_t { *c.Ptr<uint32_t>(0) = 2; return 0; });   // Adult
        rt.Override("IUserDisplayInfo", "Privileges", [](Call& c) -> uint32_t { *c.Ptr<uint64_t>(0) = Runtime::Get().NewVectorView({}); return 0; });
        // A title that waits for UserAdded (instead of reading Users) is told about the one signed-in user as soon as it subscribes.
        rt.Override("IUserAddedEventArgs", "get_User", [](Call& c) -> uint32_t { *c.Ptr<uint64_t>(0) = CurrentUserPointer(); return 0; });
        rt.Override("IUserStatics", "add_UserAdded", [](Call& c) -> uint32_t {
            Runtime& r = Runtime::Get();
            const uint64_t handler = c.Arg(0);
            const uint64_t token = r.AddDelegate(c.obj, "UserAdded", handler);
            if (auto* tok = c.Ptr<uint64_t>(1)) *tok = token;
            if (handler <= 0x10000) return 0;
            // The callee keeps its own reference; the event itself is raised later, from the dispatcher (a title may not be ready for it yet).
            const uint64_t vtable = *reinterpret_cast<uint64_t*>(handler);
            c.thread.CallGuest(GuestStep::Call(reinterpret_cast<uint64_t*>(vtable)[1], {handler}, [](NtThread&, uint64_t) { return GuestStep::Return(0); }));
            g_pending_user_added.push_back({handler, r.Pointer(c.obj, c.iface)});
            UT_INFO("winrt: UserAdded handler %#llx subscribed; raised from the dispatcher", (unsigned long long)handler);
            return 0;
        });
        // The account picker "picks" the one user straight away.
        rt.Override("IAccountPickerResult", "get_User", [](Call& c) -> uint32_t { *c.Ptr<uint64_t>(0) = CurrentUserPointer(); return 0; });
        rt.Override("ISystemUIStatics", "ShowAccountPickerAsync", [](Call& c) -> uint32_t {
            Runtime& r = Runtime::Get();
            Iface* i = r.FindInterface("IAccountPickerResult");
            Obj* result = r.NewObject("Windows.Xbox.UI.AccountPickerResult", {i});
            *c.Ptr<uint64_t>(2) = r.NewAsync(Out::Ptr, r.Pointer(*result, *i));
            return 0;
        });
        rt.Override("IUser", "get_XboxUserHash", [](Call& c) -> uint32_t { *c.Ptr<uint64_t>(0) = Runtime::Get().MakeHString(u"utsuro"); return 0; });
    }
    // Windows.Xbox.Services.XboxLiveConfiguration: the title's identity, from its manifest (<mx:XboxLive TitleId="34BA1DE0" PrimaryServiceConfigId="...">).
    {
        struct Identity { std::u16string title_id_decimal, service_config; };
        static const Identity identity = [] {
            Identity id;
            std::string text;
            if (FILE* f = std::fopen((Runtime::Get().kernel().Options().title_root + "/AppxManifest.xml").c_str(), "rb")) {
                char buf[4096];
                size_t n;
                while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
                std::fclose(f);
            }
            auto attr = [&](const char* name) {
                const std::string key = std::string(name) + "=\"";
                const size_t at = text.find(key);
                if (at == std::string::npos) return std::string();
                const size_t end = text.find('"', at + key.size());
                return text.substr(at + key.size(), end - at - key.size());
            };
            const std::string hex = attr("TitleId");
            const std::string decimal = std::to_string(std::strtoull(hex.c_str(), nullptr, 16));
            id.title_id_decimal.assign(decimal.begin(), decimal.end());
            const std::string cfg = attr("PrimaryServiceConfigId");
            id.service_config.assign(cfg.begin(), cfg.end());
            return id;
        }();
        rt.Override("IXboxLiveConfigurationStatics", "get_TitleId", [](Call& c) -> uint32_t { *c.Ptr<uint64_t>(0) = Runtime::Get().MakeHString(identity.title_id_decimal); return 0; });
        rt.Override("IXboxLiveConfigurationStatics", "get_PrimaryServiceConfigId", [](Call& c) -> uint32_t { *c.Ptr<uint64_t>(0) = Runtime::Get().MakeHString(identity.service_config); return 0; });
        rt.Override("IXboxLiveConfigurationStatics", "get_SandboxId", [](Call& c) -> uint32_t { *c.Ptr<uint64_t>(0) = Runtime::Get().MakeHString(u"RETAIL"); return 0; });
    }
    RegisterInput(rt);
    // Windows.ApplicationModel.Store.CurrentApp: the title is always licensed and never a trial.
    Iface& license = rt.DefineInterface("ILicenseInformation", "8EBCAB32-0B5A-4A8A-A0A7-DC4B4B3C0D56", {
        Get("get_ProductLicenses", Out::Ptr),
        Fn("get_IsActive", Out::Bool, [](Call& c) -> uint32_t { *c.Ptr<uint8_t>(0) = 1; return 0; }),
        Get("get_IsTrial", Out::Bool),
        Get("get_ExpirationDate", Out::I64),
        Add("add_LicenseChanged"), Remove("remove_LicenseChanged"),
    });
    Iface& current_app = rt.DefineInterface("ICurrentApp", "D52DC065-DA3F-4685-995E-9B482EB5E603", {
        Fn("get_LicenseInformation", Out::Ptr, [&license](Call& c) -> uint32_t {
            static Obj* info = nullptr;
            if (info == nullptr) info = Runtime::Get().NewObject("Windows.ApplicationModel.Store.LicenseInformation", {&license});
            ++info->refs;
            *c.Ptr<uint64_t>(0) = Runtime::Get().Pointer(*info, license);
            return 0;
        }),
        Get("get_LinkUri", Out::Ptr),
        Get("get_AppId", Out::Guid),
        Get("RequestAppPurchaseAsync", Out::Ptr),
        Get("RequestProductPurchaseAsync", Out::Ptr),
        Get("LoadListingInformationAsync", Out::Ptr),
        Get("GetAppReceiptAsync", Out::Ptr),
        Get("GetProductReceiptAsync", Out::Ptr),
    });
    rt.DefineClass("Windows.ApplicationModel.Store.CurrentApp", [](Runtime&) -> Obj* { return nullptr; }, &current_app);

    // Windows.Xbox.Management.Deployment.PackageTransferManager.Current: everything is installed, nothing is transferring.
    {
        rt.Override("IPackageTransferManagerStatics", "get_Current", [](Call& c) -> uint32_t {
            Runtime& r = Runtime::Get();
            static Obj* manager = nullptr;
            Iface* main = r.FindInterface("IPackageTransferManager");
            if (manager == nullptr) manager = r.NewObject("Windows.Xbox.Management.Deployment.PackageTransferManager", {main, r.FindInterface("IPackageTransferManagerTransferInfo"), r.FindInterface("IPackageTransferManagerForChunkSpecifiers")});
            ++manager->refs;
            *c.Ptr<uint64_t>(0) = r.Pointer(*manager, *main);
            return 0;
        });
        for (const char* name : {"IsChunkInstalled", "AreChunksInstalled"})
            rt.Override("IPackageTransferManager", name, [](Call& c) -> uint32_t { *c.Ptr<uint8_t>(1) = 1; return 0; });
        rt.Override("IPackageTransferManager", "get_TransferStatus", [](Call& c) -> uint32_t { *c.Ptr<uint32_t>(0) = 0; return 0; });
    }

    // PackageTransferWatcher.Create: a watcher that never reports anything (the title waits for chunks that are all installed).
    rt.OverrideAllNamed("IPackageTransferWatcherStatics", "Create", [](Call& c) -> uint32_t {
        Runtime& r = Runtime::Get();
        static std::atomic<int> shown {0};
        if (shown++ < 6) {
            uint64_t vt = 0;
            if (c.Arg(0) > 0x10000) SafeRead(c.Arg(0), &vt, 8);
            Obj* arg = c.Arg(0) > 0x10000 ? r.FromPointer(c.Arg(0)) : nullptr;
            UT_INFO("winrt: PackageTransferWatcher.Create(slot %u: %#llx [vtable %s, host object %s], %#llx, %#llx)", c.slot, (unsigned long long)c.Arg(0),
                    c.kernel.Describe(vt).c_str(), arg ? arg->class_name.c_str() : "none", (unsigned long long)c.Arg(1), (unsigned long long)c.Arg(2));
        }
        Iface* main = r.FindInterface("IPackageTransferWatcher");
        Obj* watcher = r.NewObject("Windows.Xbox.Management.Deployment.PackageTransferWatcher", {main, r.FindInterface("IPackageTransferWatcherTransferInfo")});
        *c.Ptr<uint64_t>(c.iface.methods[c.slot].ret_arg) = r.Pointer(*watcher, *main);
        return 0;
    });

    // add_ChunkCompleted: a watcher made for a whole package (Forza Horizon 2: PackageTransferWatcher.Create(Package.Current)) raises
    // ChunkCompleted for its chunks as they install, and titles keep their own record of installed chunks from it. FH2 loads a file
    // -- an FMOD bank, for one -- only when the chunk its file table gives for that file is in that record; with nothing raised no
    // bank loaded and it sat on a black screen after its intro. A dump has every chunk installed but carries no chunk layout, so
    // every id the title can name is raised: FH2's table uses ids from 0 to 0x22a5 (745 of them, ten apart), hence 0 .. 0x3fff.
    rt.Override("IChunkCompletedEventArgs", "get_ChunkId", [](Call& c) -> uint32_t {
        if (auto* out = c.Ptr<uint32_t>(0)) *out = static_cast<uint32_t>(c.obj.u64["chunk"]);
        return 0;
    });
    rt.Override("IPackageTransferWatcher", "add_ChunkCompleted", [](Call& c) -> uint32_t {
        constexpr uint32_t kChunks = 0x4000;
        const uint64_t handler = c.Arg(0);
        if (auto* token = c.Ptr<uint64_t>(1)) *token = 1;
        if (handler <= 0x10000) return 0;
        Runtime& r = Runtime::Get();
        const uint64_t sender = r.Pointer(c.obj, c.iface);
        Iface* args_iface = r.FindInterface("IChunkCompletedEventArgs");
        Obj* args = r.NewObject("Windows.Xbox.Management.Deployment.ChunkCompletedEventArgs", {args_iface});   // one, reused: the handler reads it and lets go
        args->refs += 1;
        const uint64_t args_ptr = r.Pointer(*args, *args_iface);
        UT_INFO("winrt: PackageTransferWatcher raises ChunkCompleted for chunks 0 .. %#x", kChunks - 1);
        auto fire = std::make_shared<std::function<GuestStep(uint32_t)>>();
        *fire = [fire, handler, sender, args, args_ptr](uint32_t chunk) -> GuestStep {
            if (chunk == kChunks) return GuestStep::Return(0);
            args->u64["chunk"] = chunk;
            ++args->refs;   // the handler may Release what it was given
            return Runtime::Get().InvokeDelegate(handler, {sender, args_ptr}, [fire, chunk](NtThread&, uint64_t) { return (*fire)(chunk + 1); });
        };
        c.thread.CallGuest((*fire)(0));
        return 0;
    });

    // Windows.Media.Devices.MediaDevice: the default audio endpoints are named, nothing else is present.
    {
        auto text = [](const char16_t* value) { return [value](Call& c) -> uint32_t { *c.Ptr<uint64_t>(0) = Runtime::Get().MakeHString(value); return 0; }; };
        auto id = [](const char16_t* value) { return [value](Call& c) -> uint32_t { *c.Ptr<uint64_t>(1) = Runtime::Get().MakeHString(value); return 0; }; };
        Method sel_capture = Method {"GetAudioCaptureSelector", Out::Ptr, 0, false, false, nullptr};
        Method sel_render = Method {"GetAudioRenderSelector", Out::Ptr, 0, false, false, nullptr};
        Method sel_video = Method {"GetVideoCaptureSelector", Out::Ptr, 0, false, false, nullptr};
        sel_capture.fn = text(u"audio-capture");
        sel_render.fn = text(u"audio-render");
        sel_video.fn = text(u"video-capture");
        Method def_capture = Method {"GetDefaultAudioCaptureId", Out::Ptr, 1, false, false, nullptr};
        Method def_render = Method {"GetDefaultAudioRenderId", Out::Ptr, 1, false, false, nullptr};
        def_capture.fn = id(u"utsuro-capture");
        def_render.fn = id(u"utsuro-render");
        Iface& media_device = rt.DefineInterface("IMediaDeviceStatics", "AA2D9A40-909F-4BBA-BF8B-0C0D296F14F0", {
            sel_capture, sel_render, sel_video, def_capture, def_render,
            Add("add_DefaultAudioCaptureDeviceChanged"), Remove("remove_DefaultAudioCaptureDeviceChanged"),
            Add("add_DefaultAudioRenderDeviceChanged"), Remove("remove_DefaultAudioRenderDeviceChanged"),
        });
        rt.DefineClass("Windows.Media.Devices.MediaDevice", [](Runtime&) -> Obj* { return nullptr; }, &media_device);
    }

    // Windows.Networking.Connectivity.NetworkInformation: there is no network profile; the title copes with that.
    Iface& net_info = rt.DefineInterface("INetworkInformationStatics", "5074F851-950D-4165-9C15-365619481EEA", {
        Get("GetConnectionProfiles", Ptr), Get("GetInternetConnectionProfile", Ptr), Get("GetLanIdentifiers", Ptr),
        Get("GetHostNames", Ptr), Result("GetProxyConfigurationAsync", Ptr, 1), Result("GetSortedEndpointPairs", Ptr, 2),
        Add("add_NetworkStatusChanged"), Remove("remove_NetworkStatusChanged"),
    });
    rt.DefineClass("Windows.Networking.Connectivity.NetworkInformation", [](Runtime&) -> Obj* { return nullptr; }, &net_info);
    // The lists a title walks are empty, not null.
    for (const char* name : {"GetConnectionProfiles", "GetLanIdentifiers", "GetHostNames"})
        rt.Override("INetworkInformationStatics", name, [](Call& c) -> uint32_t { *c.Ptr<uint64_t>(0) = Runtime::Get().NewVectorView({}); return 0; });

}

} // namespace utsuro::nt::winrt
