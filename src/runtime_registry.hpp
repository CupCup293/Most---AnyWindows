#pragma once
inline constexpr auto runtimeRegistry = R"MOST(
struct RegistryValue {
    std::u16string name;
    uint32_t type;
    std::vector<uint8_t> bytes;
};

struct RegistryKey {
    std::u16string name;
    bool deleted = false;
    std::map<std::u16string, RegistryValue> values;
};

using KeyRef = std::shared_ptr<RegistryKey>;
struct RegistryHandle {
    std::u16string path;
    KeyRef key;
    uint32_t rights;
};

static std::map<std::u16string, KeyRef> registry;
static std::map<uint32_t, RegistryHandle> registry_handles;
static uint32_t next_registry_handle = 0x20000000;

static std::u16string fold_ascii(std::u16string name)
{
    for (auto& unit : name)
        if (unit >= u'a' && unit <= u'z')
            unit -= u'a' - u'A';
    return name;
}

static uint32_t registry_handle(uint32_t handle, uint32_t rights, RegistryHandle& result)
{
    if (handle >= 0x80000000u && handle <= 0x80000003u) {
        const std::u16string path(1, char16_t(u'0' + (handle - 0x80000000u)));
        auto& key = registry[path];
        if (!key)
            key = std::make_shared<RegistryKey>();
        result = {path, key, 0xffff};
        return 0;
    }
    const auto found = registry_handles.find(handle);
    if (found == registry_handles.end())
        return 6;
    if (found->second.key->deleted)
        return 1018;
    if (rights & ~found->second.rights)
        return 5;
    result = found->second;
    return 0;
}

static bool registry_name(uint32_t address, std::u16string& name)
{
    return !address || read_guest_wide(address, name);
}

static uint32_t open_registry_key(uint32_t handle, uint32_t name, uint32_t rights,
                                  uint32_t output, uint32_t disposition)
{
    RegistryHandle parent;
    if (const auto status = registry_handle(handle, 0, parent))
        return status;
    std::u16string subkey;
    if (!output || !registry_name(name, subkey) || rights & ~(0x001f033fu | 0x02000000u))
        return 87;
    if (next_registry_handle == 0x30000000)
        return 4;
    std::vector<std::u16string> components;
    size_t start = 0;
    while (start < subkey.size()) {
        const auto end = subkey.find(u'\\', start);
        const auto component = subkey.substr(start, end - start);
        if (component.empty() || component.size() > 255)
            return 87;
        components.push_back(component);
        if (end == std::u16string::npos)
            break;
        start = end + 1;
    }
    auto path = parent.path;
    KeyRef key = parent.key;
    uint32_t created = 2;
    for (const auto& component : components) {
        path += u'\\' + fold_ascii(component);
        const auto found = registry.find(path);
        if (found != registry.end()) {
            key = found->second;
        } else {
            if (!disposition)
                return 2;
            if (!(parent.rights & 4))
                return 5;
            key = std::make_shared<RegistryKey>();
            key->name = component;
            registry.emplace(path, key);
            created = 1;
        }
    }
    const auto result = next_registry_handle++;
    registry_handles.emplace(result, RegistryHandle{path, key, rights & 0x02000000u ? 0xffffu : rights});
    store32(output, result);
    if (disposition != UINT32_MAX && disposition)
        store32(disposition, created);
    return 0;
}

static uint32_t win_RegOpenKeyExW(uint32_t *sp)
{
    const uint32_t root = pop32(sp), name = pop32(sp), options = pop32(sp);
    const uint32_t access = pop32(sp), output = pop32(sp);
    return options ? 87 : open_registry_key(root, name, access, output, 0);
}

static uint32_t win_RegCreateKeyExW(uint32_t *sp)
{
    const uint32_t root = pop32(sp), name = pop32(sp), reserved = pop32(sp), class_name = pop32(sp);
    const uint32_t options = pop32(sp), access = pop32(sp), security = pop32(sp);
    const uint32_t output = pop32(sp), disposition = pop32(sp);
    if (reserved || class_name || security || options > 1)
        return 87;
    return open_registry_key(root, name, access, output, disposition ? disposition : UINT32_MAX);
}

static uint32_t win_RegCloseKey(uint32_t *sp)
{
    const uint32_t handle = pop32(sp);
    return registry_handles.erase(handle) || (handle >= 0x80000000u && handle <= 0x80000003u) ? 0 : 6;
}

static uint32_t win_RegSetValueExW(uint32_t *sp)
{
    const uint32_t handle = pop32(sp), name = pop32(sp), reserved = pop32(sp);
    const uint32_t type = pop32(sp), data = pop32(sp), size = pop32(sp);
    RegistryHandle key;
    std::u16string value_name;
    if (const auto status = registry_handle(handle, 2, key))
        return status;
    if (reserved || !registry_name(name, value_name) || (size && !data) || size > 1024 * 1024)
        return 87;
    RegistryValue value{value_name, type, {}};
    if (size) {
        const auto* start = memory(data, size, 0);
        value.bytes.assign(start, start + size);
    }
    key.key->values.insert_or_assign(fold_ascii(value_name), std::move(value));
    return 0;
}

static uint32_t copy_registry_value(const RegistryValue& value, uint32_t type, uint32_t data, uint32_t size)
{
    if (data && !size)
        return 87;
    if (type)
        store32(type, value.type);
    if (!size)
        return 0;
    const uint32_t capacity = data ? load32(size) : 0;
    store32(size, value.bytes.size());
    if (data && capacity < value.bytes.size())
        return 234;
    if (data && !value.bytes.empty())
        memcpy(memory(data, value.bytes.size(), 1), value.bytes.data(), value.bytes.size());
    return 0;
}

static uint32_t win_RegQueryValueExW(uint32_t *sp)
{
    const uint32_t handle = pop32(sp), name = pop32(sp), reserved = pop32(sp);
    const uint32_t type = pop32(sp), data = pop32(sp), size = pop32(sp);
    RegistryHandle key;
    std::u16string value_name;
    if (const auto status = registry_handle(handle, 1, key))
        return status;
    if (reserved || !registry_name(name, value_name))
        return 87;
    const auto found = key.key->values.find(fold_ascii(value_name));
    return found == key.key->values.end() ? 2 : copy_registry_value(found->second, type, data, size);
}

static uint32_t win_RegDeleteValueW(uint32_t *sp)
{
    const uint32_t handle = pop32(sp), name = pop32(sp);
    RegistryHandle key;
    std::u16string value_name;
    if (const auto status = registry_handle(handle, 2, key))
        return status;
    if (!registry_name(name, value_name))
        return 87;
    return key.key->values.erase(fold_ascii(value_name)) ? 0 : 2;
}

static uint32_t win_RegEnumValueW(uint32_t *sp)
{
    const uint32_t handle = pop32(sp), index = pop32(sp), name = pop32(sp), length = pop32(sp);
    const uint32_t reserved = pop32(sp), type = pop32(sp), data = pop32(sp), size = pop32(sp);
    RegistryHandle key;
    if (const auto status = registry_handle(handle, 1, key))
        return status;
    if (!name || !length || reserved || (data && !size))
        return 87;
    if (index >= key.key->values.size())
        return 259;
    const auto& value = std::next(key.key->values.begin(), index)->second;
    if (load32(length) <= value.name.size())
        return 234;
    const auto status = copy_registry_value(value, type, data, size);
    if (!status) {
        store_wide(name, value.name.data(), value.name.size());
        store32(length, value.name.size());
    }
    return status;
}

static uint32_t win_RegEnumKeyW(uint32_t *sp)
{
    const uint32_t handle = pop32(sp), index = pop32(sp), name = pop32(sp), capacity = pop32(sp);
    RegistryHandle key;
    if (const auto status = registry_handle(handle, 8, key))
        return status;
    if (!name)
        return 87;
    const auto prefix = key.path + u'\\';
    uint32_t item = 0;
    for (auto it = registry.lower_bound(prefix); it != registry.end() && it->first.starts_with(prefix); ++it) {
        if (it->first.find(u'\\', prefix.size()) != std::u16string::npos)
            continue;
        if (item++ != index)
            continue;
        const auto& label = it->second->name;
        if (capacity <= label.size())
            return 234;
        store_wide(name, label.data(), label.size());
        return 0;
    }
    return 259;
}

static uint32_t win_RegDeleteKeyW(uint32_t *sp)
{
    const uint32_t handle = pop32(sp), name = pop32(sp);
    RegistryHandle parent;
    std::u16string subkey;
    if (const auto status = registry_handle(handle, 0, parent))
        return status;
    if (!registry_name(name, subkey) || subkey.empty() || subkey.front() == u'\\')
        return 87;
    if (subkey.back() == u'\\')
        subkey.pop_back();
    if (subkey.empty())
        return 87;
    const auto path = parent.path + u'\\' + fold_ascii(subkey);
    const auto found = registry.find(path);
    if (found == registry.end())
        return 2;
    const auto prefix = path + u'\\';
    const auto child = registry.lower_bound(prefix);
    if (child != registry.end() && child->first.starts_with(prefix))
        return 5;
    found->second->deleted = true;
    registry.erase(found);
    return 0;
}
)MOST";
