// winrt_classes.cpp -- the Windows.Xbox.* classes the titles activate. Interface and method
// order follow WinDurango's IDL notes; a method without a body uses the generic default
// (getters write zero, setters succeed, events hand out tokens).
#include "nt/winrt.h"
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
    // One signed-in user, id 1, who plays on the one pad.
    {
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
        Iface* main = r.FindInterface("IPackageTransferWatcher");
        Obj* watcher = r.NewObject("Windows.Xbox.Management.Deployment.PackageTransferWatcher", {main, r.FindInterface("IPackageTransferWatcherTransferInfo")});
        *c.Ptr<uint64_t>(c.iface.methods[c.slot].ret_arg) = r.Pointer(*watcher, *main);
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
