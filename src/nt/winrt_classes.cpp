// winrt_classes.cpp -- the Windows.Xbox.* classes the titles activate. Interface and method
// order follow WinDurango's IDL notes; a method without a body uses the generic default
// (getters write zero, setters succeed, events hand out tokens).
#include "nt/winrt.h"

#include "engine/log.h"

namespace utsuro::nt::winrt {

namespace {

constexpr Out None = Out::None, Bool = Out::Bool, I32 = Out::I32, I64 = Out::I64, Ptr = Out::Ptr, GuidT = Out::Guid;

Method Get(const char* name, Out out) { return Method {name, out, 0, false, false, nullptr}; }
Method Put(const char* name) { return Method {name, Out::None, 0, false, false, nullptr}; }
Method Add(const char* name) { return Method {name, Out::None, 0, true, false, nullptr}; }
Method Remove(const char* name) { return Method {name, Out::None, 0, false, true, nullptr}; }
Method Result(const char* name, Out out, int in_params) { return Method {name, out, in_params, false, false, nullptr}; }
Method Fn(const char* name, Out out, HostFn fn) { return Method {name, out, 0, false, false, std::move(fn)}; }

#include "nt/winrt_generated.inc"

} // namespace

void RegisterClasses(Runtime& rt) {
    RegisterGenerated(rt);
    RegisterCoreClasses(rt);
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
