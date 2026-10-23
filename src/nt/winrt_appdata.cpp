// winrt_appdata.cpp -- Windows.Storage.ApplicationData and the StorageFolders it hands out
// (LocalFolder, TemporaryFolder, ...). The real implementation talks to a storage service over
// ALPC; here each folder is a plain host directory below the save root and the title is given its
// path, which it then uses with ordinary file calls (see Kernel::ResolvePath).
#include "nt/winrt.h"

#include "engine/log.h"

#include <filesystem>
#include <fstream>
#include <map>

namespace utsuro::nt::winrt {

namespace {

constexpr Out None = Out::None, Bool = Out::Bool, I32 = Out::I32, I64 = Out::I64, Ptr = Out::Ptr;

Method Get(const char* name, Out out) { return Method {name, out, 0, false, false, nullptr}; }
Method Fn(const char* name, Out out, HostFn fn) { return Method {name, out, 0, false, false, std::move(fn)}; }
const char* kPackageRoot = "C:\\Users\\Default\\AppData\\Local\\Packages\\Utsuro";

Iface* g_item = nullptr;
Iface* g_folder = nullptr;

/// A StorageFolder for the NT path `nt`; `create` makes its directory on the host first.
uint64_t NewFolder(Call& c, const std::string& nt, const char* name, bool create) {
    Runtime& r = Runtime::Get();
    if (create) {
        std::string host;
        bool exists = false;
        if (c.kernel.ResolvePath(nt, &host, &exists, true)) {
            std::error_code ec;
            std::filesystem::create_directories(host, ec);
        }
    }
    Obj* o = r.NewObject("Windows.Storage.StorageFolder", {g_folder, g_item});
    o->str["path"] = nt;
    o->str["name"] = name;
    return r.Pointer(*o, *g_folder);
}

uint32_t StringGetter(Call& c, const char* key) {
    const std::string& v = c.obj.str[key];
    *c.Ptr<uint64_t>(0) = Runtime::Get().MakeHString(std::u16string(v.begin(), v.end()));
    return 0;
}

} // namespace

void RegisterAppData(Runtime& rt) {
    // The console's own, much smaller, shapes of these interfaces (WinDurango's EraCoreApplication.h).
    g_item = &rt.DefineInterface("IStorageItem", "4207A996-CA2F-42F7-BDE8-8B10457A7F30", {
        Fn("get_Name", Ptr, [](Call& c) -> uint32_t { return StringGetter(c, "name"); }),
        Fn("get_Path", Ptr, [](Call& c) -> uint32_t { return StringGetter(c, "path"); }),
        Get("get_Attributes", I32),
        Get("get_DateCreated", I64),
        Get("IsOfType", Bool),
    });
    g_folder = &rt.DefineInterface("IStorageFolder", "72D1CB78-B3EF-4F75-A80B-6FD9DAE2944B", {});

    Iface& data = rt.DefineInterface("IApplicationData", "C3DA6FB7-B744-4B45-B0B8-223A0938D0DC", {
        Fn("get_LocalFolder", Ptr, [](Call& c) -> uint32_t {
            *c.Ptr<uint64_t>(0) = NewFolder(c, std::string(kPackageRoot) + "\\LocalState", "LocalState", true);
            return 0;
        }),
    });
    Iface& data_statics = rt.DefineInterface("IApplicationDataStatics", "5612147B-E843-45E3-94D8-06169E3C8E17", {
        Fn("get_Current", Ptr, [&data](Call& c) -> uint32_t {
            static Obj* current = nullptr;
            Runtime& r = Runtime::Get();
            if (current == nullptr) current = r.NewObject("Windows.Storage.ApplicationData", {&data});
            ++current->refs;
            *c.Ptr<uint64_t>(0) = r.Pointer(*current, data);
            return 0;
        }),
    });
    rt.DefineClass("Windows.Storage.ApplicationData", [](Runtime&) -> Obj* { return nullptr; }, &data_statics);

    // Windows.ApplicationModel.Package.Current: the install folder is the title's mount.
    Iface& package = rt.DefineInterface("IPackage", "163C792F-BD75-413C-BF23-B1FE7B95D825", {
        Get("get_Id", Ptr),
        Fn("get_InstalledLocation", Ptr, [](Call& c) -> uint32_t {
            *c.Ptr<uint64_t>(0) = NewFolder(c, "G:\\", "Mount", false);
            return 0;
        }),
        Get("get_IsFramework", Bool),
    });
    Iface& package_statics = rt.DefineInterface("IPackageStatics", "4E534BDF-2960-4878-97A4-9624DEB72F2D", {
        Fn("get_Current", Ptr, [&package](Call& c) -> uint32_t {
            static Obj* current = nullptr;
            Runtime& r = Runtime::Get();
            if (current == nullptr) current = r.NewObject("Windows.ApplicationModel.Package", {&package});
            ++current->refs;
            *c.Ptr<uint64_t>(0) = r.Pointer(*current, package);
            return 0;
        }),
    });
    rt.DefineClass("Windows.ApplicationModel.Package", [](Runtime&) -> Obj* { return nullptr; }, &package_statics);

    // Windows.Xbox.Networking.SecureDeviceAssociationTemplate.GetTemplateByName: the templates the title's manifest declares. The
    // title registers for incoming associations on them at start-up; there is no secure-device network behind them here.
    rt.Override("ISecureDeviceAssociationTemplateStatics", "GetTemplateByName", [](Call& c) -> uint32_t {
        static std::mutex m;
        static std::map<std::u16string, Obj*> templates;
        std::scoped_lock lock {m};
        Runtime& r = Runtime::Get();
        const std::u16string name = Runtime::ReadHString(c.Arg(0));
        Obj*& o = templates[name];
        Iface* iface = r.FindInterface("ISecureDeviceAssociationTemplate");
        if (o == nullptr) {
            o = r.NewObject("Windows.Xbox.Networking.SecureDeviceAssociationTemplate", {iface});
            o->str["name"] = std::string(name.begin(), name.end());
        }
        ++o->refs;
        *c.Ptr<uint64_t>(1) = r.Pointer(*o, *iface);
        return 0;
    });
    // The socket descriptions behind a template, read from the title's own manifest.
    auto manifest_text = [](Kernel& k) {
        static std::string text;
        if (text.empty()) {
            std::string host;
            bool exists = false;
            if (k.ResolvePath("G:\\AppxManifest.xml", &host, &exists) && exists) {
                std::ifstream f(host, std::ios::binary);
                text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            }
        }
        return text;
    };
    auto attribute = [](const std::string& tag, const std::string& name) -> std::string {
        const size_t at = tag.find(" " + name + "=\"");
        if (at == std::string::npos) return {};
        const size_t begin = at + name.size() + 3, end = tag.find('"', begin);
        return end == std::string::npos ? std::string() : tag.substr(begin, end - begin);
    };
    auto find_tag = [](const std::string& text, const std::string& element, const std::string& name) -> std::string {
        size_t pos = 0;
        const std::string open = "<mx:" + element + " ";
        while ((pos = text.find(open, pos)) != std::string::npos) {
            const size_t end = text.find('>', pos);
            const std::string tag = text.substr(pos, end - pos);
            if (tag.find(" Name=\"" + name + "\"") != std::string::npos) return tag;
            pos = end;
        }
        return {};
    };
    static Iface* socket_iface = &rt.DefineInterface("ISecureDeviceSocketDescription", "9835ED01-7B1E-41FC-B64D-7D20448892D6", {
        Fn("get_Name", Ptr, [](Call& c) -> uint32_t { return StringGetter(c, "name"); }),
        Fn("get_IpProtocol", I32, [](Call& c) -> uint32_t { *c.Ptr<uint32_t>(0) = static_cast<uint32_t>(c.obj.u64["protocol"]); return 0; }),
        Fn("get_BoundPortRangeLower", I32, [](Call& c) -> uint32_t { *c.Ptr<uint32_t>(0) = static_cast<uint32_t>(c.obj.u64["lower"]); return 0; }),
        Fn("get_BoundPortRangeUpper", I32, [](Call& c) -> uint32_t { *c.Ptr<uint32_t>(0) = static_cast<uint32_t>(c.obj.u64["upper"]); return 0; }),
        Get("get_AllowedUsages", I32),
    });
    auto socket = [manifest_text, attribute, find_tag](Call& c, bool initiator) -> uint32_t {
        Runtime& r = Runtime::Get();
        const std::string text = manifest_text(c.kernel);
        const std::string tmpl = find_tag(text, "SecureDeviceAssociationTemplate", c.obj.str["name"]);
        const std::string socket_name = attribute(tmpl, initiator ? "InitiatorSocketDescription" : "AcceptorSocketDescription");
        const std::string desc = find_tag(text, "SocketDescription", socket_name);
        Obj* o = r.NewObject("Windows.Xbox.Networking.SecureDeviceSocketDescription", {socket_iface});
        o->str["name"] = socket_name;
        o->u64["protocol"] = attribute(desc, "SecureIpProtocol") == "Tcp" ? 0 : 1;
        const std::string ports = attribute(desc, "BoundPort");
        const size_t dash = ports.find('-');
        o->u64["lower"] = static_cast<uint64_t>(std::atoi(ports.c_str()));
        o->u64["upper"] = static_cast<uint64_t>(dash == std::string::npos ? std::atoi(ports.c_str()) : std::atoi(ports.c_str() + dash + 1));
        *c.Ptr<uint64_t>(0) = r.Pointer(*o, *socket_iface);
        return 0;
    };
    rt.Override("ISecureDeviceAssociationTemplate", "get_InitiatorSocketDescription", [socket](Call& c) -> uint32_t { return socket(c, true); });
    rt.Override("ISecureDeviceAssociationTemplate", "get_AcceptorSocketDescription", [socket](Call& c) -> uint32_t { return socket(c, false); });
    rt.Override("ISecureDeviceAssociationTemplate", "get_Name", [](Call& c) -> uint32_t {
        const std::string& n = c.obj.str["name"];
        *c.Ptr<uint64_t>(0) = Runtime::Get().MakeHString(std::u16string(n.begin(), n.end()));
        return 0;
    });
}

} // namespace utsuro::nt::winrt
