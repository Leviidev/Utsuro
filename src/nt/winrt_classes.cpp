// winrt_classes.cpp -- the Windows.Xbox.* classes the titles activate. Interface and method
// order follow WinDurango's IDL notes; a method without a body uses the generic default
// (getters write zero, setters succeed, events hand out tokens).
#include "nt/winrt.h"

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
        rt.Override("IUser", "get_XboxUserHash", [](Call& c) -> uint32_t { *c.Ptr<uint64_t>(0) = Runtime::Get().MakeHString(u"utsuro"); return 0; });
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

    // Windows.Networking.Connectivity.NetworkInformation: there is no network profile; the title copes with that.
    Iface& net_info = rt.DefineInterface("INetworkInformationStatics", "5074F851-950D-4165-9C15-365619481EEA", {
        Get("GetConnectionProfiles", Ptr), Get("GetInternetConnectionProfile", Ptr), Get("GetLanIdentifiers", Ptr),
        Get("GetHostNames", Ptr), Result("GetProxyConfigurationAsync", Ptr, 1), Result("GetSortedEndpointPairs", Ptr, 2),
        Add("add_NetworkStatusChanged"), Remove("remove_NetworkStatusChanged"),
    });
    rt.DefineClass("Windows.Networking.Connectivity.NetworkInformation", [](Runtime&) -> Obj* { return nullptr; }, &net_info);

}

} // namespace utsuro::nt::winrt
