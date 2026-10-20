// winrt_collections.cpp -- host implementations of the generic WinRT collections. A
// parameterized interface (IVectorView<T>) has an IID that depends on T, so the object adopts
// whichever IID the title asks for first.
#include "nt/winrt.h"

#include "engine/log.h"

namespace utsuro::nt::winrt {

namespace {
Iface* g_vector_view = nullptr;
Iface* g_iterable_vector = nullptr;
Iface* g_iterator_vector = nullptr;

Iface* g_map_view = nullptr;
Iface* g_iterable_map = nullptr;
Iface* g_iterator_map = nullptr;
Iface* g_key_value_pair = nullptr;
}

void RegisterCollections(Runtime& rt) {
    auto fn = [](const char* name, HostFn f) { Method m; m.name = name; m.fn = std::move(f); return m; };

    g_vector_view = &rt.DefineInterface("IVectorView", "00000000-0000-0000-0000-000000000000", {
        fn("GetAt", [](Call& c) -> uint32_t {
            const uint64_t i = c.Arg(0);
            if (i >= c.obj.items.size()) { *c.Ptr<uint64_t>(1) = 0; return 0x8000000Bu; }   // E_BOUNDS
            *c.Ptr<uint64_t>(1) = c.obj.items[i];
            return 0;
        }),
        fn("get_Size", [](Call& c) -> uint32_t { *c.Ptr<uint32_t>(0) = static_cast<uint32_t>(c.obj.items.size()); return 0; }),
        fn("IndexOf", [](Call& c) -> uint32_t {
            *c.Ptr<uint8_t>(2) = 0;
            *c.Ptr<uint32_t>(1) = 0;
            for (size_t i = 0; i < c.obj.items.size(); ++i)
                if (c.obj.items[i] == c.Arg(0)) { *c.Ptr<uint32_t>(1) = static_cast<uint32_t>(i); *c.Ptr<uint8_t>(2) = 1; break; }
            return 0;
        }),
        fn("GetMany", [](Call& c) -> uint32_t {
            const uint64_t start = c.Arg(0), cap = c.Arg(1);
            uint32_t n = 0;
            for (uint64_t i = start; i < c.obj.items.size() && n < cap; ++i, ++n) c.Ptr<uint64_t>(2)[n] = c.obj.items[i];
            *c.Ptr<uint32_t>(3) = n;
            return 0;
        }),
    }, true);

    g_iterable_vector = &rt.DefineInterface("IIterable_Vector", "00000000-0000-0000-0000-000000000000", {
        fn("First", [](Call& c) -> uint32_t {
            Runtime& r = Runtime::Get();
            Obj* iter = r.NewObject("Windows.Foundation.Collections.VectorIterator", {g_iterator_vector});
            iter->items = c.obj.items;
            iter->u64["pos"] = 0;
            *c.Ptr<uint64_t>(0) = r.Pointer(*iter, *g_iterator_vector);
            return 0;
        }),
    }, true);

    g_iterator_vector = &rt.DefineInterface("IIterator_Vector", "00000000-0000-0000-0000-000000000000", {
        fn("get_Current", [](Call& c) -> uint32_t {
            const uint64_t pos = c.obj.u64["pos"];
            if (pos >= c.obj.items.size()) {
                *c.Ptr<uint64_t>(0) = 0;
                return 0x8000000Bu;   // E_BOUNDS
            }
            *c.Ptr<uint64_t>(0) = c.obj.items[pos];
            return 0;
        }),
        fn("get_HasCurrent", [](Call& c) -> uint32_t {
            const uint64_t pos = c.obj.u64["pos"];
            *c.Ptr<uint8_t>(0) = (pos < c.obj.items.size()) ? 1 : 0;
            return 0;
        }),
        fn("MoveNext", [](Call& c) -> uint32_t {
            c.obj.u64["pos"]++;
            const uint64_t pos = c.obj.u64["pos"];
            *c.Ptr<uint8_t>(0) = (pos < c.obj.items.size()) ? 1 : 0;
            return 0;
        }),
        fn("GetMany", [](Call& c) -> uint32_t {
            *c.Ptr<uint32_t>(1) = 0;
            return 0;
        }),
    }, true);

    // IMapView<K, V>: the host's maps are keyed by HSTRING contents; values are interface pointers.
    g_map_view = &rt.DefineInterface("IMapView", "2cfeec4f-e261-5f4c-aee1-c78518e9d5b9", {
        fn("Lookup", [](Call& c) -> uint32_t {
            const std::u16string key = Runtime::ReadHString(c.Arg(0));
            for (size_t i = 0; i + 1 < c.obj.items.size(); i += 2)
                if (Runtime::ReadHString(c.obj.items[i]) == key) { *c.Ptr<uint64_t>(1) = c.obj.items[i + 1]; return 0; }
            *c.Ptr<uint64_t>(1) = 0;
            return 0x8000000Bu;   // E_BOUNDS
        }),
        fn("get_Size", [](Call& c) -> uint32_t { *c.Ptr<uint32_t>(0) = static_cast<uint32_t>(c.obj.items.size() / 2); return 0; }),
        fn("HasKey", [](Call& c) -> uint32_t {
            const std::u16string key = Runtime::ReadHString(c.Arg(0));
            *c.Ptr<uint8_t>(1) = 0;
            for (size_t i = 0; i + 1 < c.obj.items.size(); i += 2)
                if (Runtime::ReadHString(c.obj.items[i]) == key) { *c.Ptr<uint8_t>(1) = 1; break; }
            return 0;
        }),
        fn("Split", [](Call& c) -> uint32_t { *c.Ptr<uint64_t>(0) = 0; *c.Ptr<uint64_t>(1) = 0; return 0; }),
    }, false);

    // IIterable<IKeyValuePair<K, V>> for MapView
    g_iterable_map = &rt.DefineInterface("IIterable_Map", "3c9ffa92-5123-5ac4-b111-03c215f0c51c", {
        fn("First", [](Call& c) -> uint32_t {
            Runtime& r = Runtime::Get();
            Obj* iter = r.NewObject("Windows.Foundation.Collections.MapIterator", {g_iterator_map});
            iter->items = c.obj.items;
            iter->u64["pos"] = 0;
            *c.Ptr<uint64_t>(0) = r.Pointer(*iter, *g_iterator_map);
            return 0;
        }),
    }, false);

    // IIterator<IKeyValuePair<K, V>>
    g_iterator_map = &rt.DefineInterface("IIterator_Map", "d4b35ccf-4f39-4498-80a1-77b6c096e8b5", {
        fn("get_Current", [](Call& c) -> uint32_t {
            const uint64_t pos = c.obj.u64["pos"];
            if (pos >= c.obj.items.size() / 2) {
                *c.Ptr<uint64_t>(0) = 0;
                return 0x8000000Bu;   // E_BOUNDS
            }
            Runtime& r = Runtime::Get();
            Obj* kv = r.NewObject("Windows.Foundation.Collections.KeyValuePair", {g_key_value_pair});
            kv->u64["key"] = c.obj.items[pos * 2];
            kv->u64["val"] = c.obj.items[pos * 2 + 1];
            *c.Ptr<uint64_t>(0) = r.Pointer(*kv, *g_key_value_pair);
            return 0;
        }),
        fn("get_HasCurrent", [](Call& c) -> uint32_t {
            const uint64_t pos = c.obj.u64["pos"];
            *c.Ptr<uint8_t>(0) = (pos < c.obj.items.size() / 2) ? 1 : 0;
            return 0;
        }),
        fn("MoveNext", [](Call& c) -> uint32_t {
            c.obj.u64["pos"]++;
            const uint64_t pos = c.obj.u64["pos"];
            *c.Ptr<uint8_t>(0) = (pos < c.obj.items.size() / 2) ? 1 : 0;
            return 0;
        }),
        fn("GetMany", [](Call& c) -> uint32_t {
            *c.Ptr<uint32_t>(1) = 0;
            return 0;
        }),
    }, true);

    // IKeyValuePair<K, V>
    g_key_value_pair = &rt.DefineInterface("IKeyValuePair", "f81b1b56-7613-4ee4-bc05-1fab5de5c07e", {
        fn("get_Key", [](Call& c) -> uint32_t {
            *c.Ptr<uint64_t>(0) = c.obj.u64["key"];
            return 0;
        }),
        fn("get_Value", [](Call& c) -> uint32_t {
            *c.Ptr<uint64_t>(0) = c.obj.u64["val"];
            return 0;
        }),
    }, true);
}

uint64_t Runtime::NewMapView(std::vector<uint64_t> key_value_pairs) {
    Obj* o = NewObject("Windows.Foundation.Collections.MapView", {g_map_view, g_iterable_map});
    o->items = std::move(key_value_pairs);
    return Pointer(*o, *g_map_view);
}

uint64_t Runtime::NewVectorView(std::vector<uint64_t> items) {
    Obj* o = NewObject("Windows.Foundation.Collections.VectorView", {g_vector_view, g_iterable_vector});
    o->items = std::move(items);
    return Pointer(*o, *g_vector_view);
}

} // namespace utsuro::nt::winrt
