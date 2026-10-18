// winrt_storage.cpp -- Windows.Xbox.Storage: connected storage, the console's save data. The
// host keeps each container as a directory of blobs below the save root.
#include "nt/winrt.h"

#include "engine/log.h"

namespace utsuro::nt::winrt {

namespace {

Obj* g_space = nullptr;

uint64_t SpacePointer() {
    Runtime& rt = Runtime::Get();
    if (g_space == nullptr) {
        g_space = rt.NewObject("Windows.Xbox.Storage.ConnectedStorageSpace",
                               {rt.FindInterface("IConnectedStorageSpace"), rt.FindInterface("IConnectedStorageSpace2")});
    }
    ++g_space->refs;
    return rt.Pointer(*g_space, *rt.FindInterface("IConnectedStorageSpace"));
}

int OutIndex(Call& c) { return c.iface.methods[c.slot].ret_arg; }

} // namespace

void RegisterStorage(Runtime& rt) {
    auto space_async = [](Call& c) -> uint32_t {
        *c.Ptr<uint64_t>(OutIndex(c)) = Runtime::Get().NewAsync(Out::Ptr, SpacePointer());
        return 0;
    };
    rt.OverrideAllNamed("IConnectedStorageSpaceStatics", "GetForUserAsync", space_async);
    rt.OverrideAllNamed("IConnectedStorageSpaceStatics", "GetForMachineAsync", space_async);
    rt.OverrideAllNamed("IConnectedStorageSpaceStatics2", "GetSyncOnDemandForUserAsync", space_async);

    rt.Override("IConnectedStorageSpace", "get_User", [](Call& c) -> uint32_t { *c.Ptr<uint64_t>(0) = CurrentUserPointer(); return 0; });
    rt.Override("IConnectedStorageSpace", "get_ServiceConfigurationId", [](Call& c) -> uint32_t {
        *c.Ptr<uint64_t>(0) = Runtime::Get().MakeHString(u"00000000-0000-0000-0000-000000000000");
        return 0;
    });
    rt.Override("IConnectedStorageSpace", "CreateContainer", [](Call& c) -> uint32_t {
        Runtime& r = Runtime::Get();
        Obj* o = r.NewObject("Windows.Xbox.Storage.ConnectedStorageContainer",
                             {r.FindInterface("IConnectedStorageContainer"), r.FindInterface("IConnectedStorageContainer2")});
        o->str["name"] = Utf16ToUtf8(Runtime::ReadHString(c.Arg(0)));
        *c.Ptr<uint64_t>(1) = r.Pointer(*o, *r.FindInterface("IConnectedStorageContainer"));
        UT_INFO("storage: container '%s'", o->str["name"].c_str());
        return 0;
    });
    rt.Override("IConnectedStorageSpace", "CreateContainerInfoQuery", [](Call& c) -> uint32_t {
        Runtime& r = Runtime::Get();
        Obj* o = r.NewObject("Windows.Xbox.Storage.ContainerInfoQueryResult",
                             {r.FindInterface("IContainerInfoQueryResult"), r.FindInterface("IContainerInfo2QueryResult")});
        *c.Ptr<uint64_t>(1) = r.Pointer(*o, *r.FindInterface("IContainerInfoQueryResult"));
        return 0;
    });
    rt.Override("IConnectedStorageSpace", "GetRemainingBytesInQuotaAsync", [](Call& c) -> uint32_t {
        *c.Ptr<uint64_t>(0) = Runtime::Get().NewAsync(Out::I32, 0x40000000);
        return 0;
    });
    rt.Override("IConnectedStorageSpace2", "GetRemainingBytesInQuota64Async", [](Call& c) -> uint32_t {
        *c.Ptr<uint64_t>(0) = Runtime::Get().NewAsync(Out::I64, 0x40000000ull);
        return 0;
    });

    rt.Override("IConnectedStorageContainer", "get_Name", [](Call& c) -> uint32_t {
        *c.Ptr<uint64_t>(0) = Runtime::Get().MakeHString(Utf8ToUtf16(c.obj.str["name"]));
        return 0;
    });
    rt.Override("IConnectedStorageContainer", "get_OwningSpace", [](Call& c) -> uint32_t { *c.Ptr<uint64_t>(0) = SpacePointer(); return 0; });
    // Nothing stored yet: reading returns no blobs.
    rt.Override("IConnectedStorageContainer", "GetAsync", [](Call& c) -> uint32_t {
        *c.Ptr<uint64_t>(1) = Runtime::Get().NewAsync(Out::Ptr, Runtime::Get().NewMapView({}));
        return 0;
    });
    rt.Override("IConnectedStorageContainer2", "CreateBlobInfoQuery", [](Call& c) -> uint32_t {
        Runtime& r = Runtime::Get();
        Obj* o = r.NewObject("Windows.Xbox.Storage.BlobInfoQueryResult", {r.FindInterface("IBlobInfoQueryResult")});
        *c.Ptr<uint64_t>(1) = r.Pointer(*o, *r.FindInterface("IBlobInfoQueryResult"));
        return 0;
    });

    auto empty_vector = [](Call& c) -> uint32_t {
        *c.Ptr<uint64_t>(OutIndex(c)) = Runtime::Get().NewAsync(Out::Ptr, Runtime::Get().NewVectorView({}));
        return 0;
    };
    rt.OverrideAllNamed("IContainerInfoQueryResult", "GetContainerInfoAsync", empty_vector);
    rt.OverrideAllNamed("IContainerInfo2QueryResult", "GetContainerInfo2Async", empty_vector);
    rt.OverrideAllNamed("IBlobInfoQueryResult", "GetBlobInfoAsync", empty_vector);
}

} // namespace utsuro::nt::winrt
