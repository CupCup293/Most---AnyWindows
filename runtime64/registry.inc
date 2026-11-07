typedef struct RegistryValue {
    struct RegistryValue *next;
    char *name;
    DWORD type, size;
    unsigned char data[];
} RegistryValue;

typedef struct RegistryKey {
    struct RegistryKey *next;
    char *path;
    RegistryValue *values;
} RegistryKey;

static RegistryKey *registry;

static RegistryKey *reg_handle(void *handle)
{
    for (RegistryKey *item = registry; item; item = item->next)
        if (item == handle)
            return item;
    return NULL;
}
static int reg_path(void *root, const char *subkey, char *path, size_t capacity)
{
    const char *prefix;
    RegistryKey *parent = reg_handle(root);
    if (parent)
        prefix = parent->path;
    else {
        switch ((DWORD)(uintptr_t)root) {
        case 0x80000000: prefix = "HKCR"; break;
        case 0x80000001: prefix = "HKCU"; break;
        case 0x80000002: prefix = "HKLM"; break;
        case 0x80000003: prefix = "HKU"; break;
        default: return 0;
        }
    }
    int n = subkey && *subkey ? snprintf(path, capacity, "%s\\%s", prefix, subkey)
                              : snprintf(path, capacity, "%s", prefix);
    return n >= 0 && (size_t)n < capacity;
}

static RegistryKey *lookup_key(void *root, const char *subkey, int create, DWORD *disposition)
{
    char path[1024];
    if (!reg_path(root, subkey, path, sizeof(path)))
        return NULL;
    for (RegistryKey *item = registry; item; item = item->next) {
        if (!strcasecmp(item->path, path)) {
            if (disposition) *disposition = 2;
            return item;
        }
    }
    if (!create)
        return NULL;
    RegistryKey *item = calloc(1, sizeof(*item));
    if (!item)
        return NULL;
    item->path = strdup(path);
    if (!item->path) {
        free(item);
        return NULL;
    }
    item->next = registry;
    registry = item;
    if (disposition) *disposition = 1;
    return item;
}
WIN int32_t win_RegOpenKeyExA(void *root, const char *subkey, DWORD options, DWORD access, void **output)
{
    (void)options; (void)access;
    if (!output)
        return 87;
    *output = lookup_key(root, subkey, 0, NULL);
    return *output ? 0 : 2;
}
WIN int32_t win_RegCreateKeyExA(void *root, const char *subkey, DWORD reserved, const char *class_name,
                                    DWORD options, DWORD access, void *security, void **output, DWORD *disposition)
{
    (void)reserved; (void)class_name; (void)options; (void)access; (void)security;
    if (!output)
        return 87;
    *output = lookup_key(root, subkey, 1, disposition);
    return *output ? 0 : 8;
}
WIN int32_t win_RegCloseKey(void *key)
{
    return reg_handle(key) || ((DWORD)(uintptr_t)key >= 0x80000000 &&
           (DWORD)(uintptr_t)key <= 0x80000003) ? 0 : 6;
}
static RegistryValue **reg_value(RegistryKey *key, const char *name)
{
    RegistryValue **link = &key->values;
    while (*link && strcasecmp((*link)->name, name ? name : ""))
        link = &(*link)->next;
    return link;
}
WIN int32_t win_RegSetValueExA(void *handle, const char *name, DWORD reserved, DWORD type,
                                   const void *data, DWORD size)
{
    (void)reserved;
    RegistryKey *key = reg_handle(handle);
    if (!key)
        return 6;
    if ((!data && size) || size > 1024 * 1024)
        return 87;
    RegistryValue **link = reg_value(key, name);
    RegistryValue *item = malloc(sizeof(*item) + size);
    if (!item)
        return 8;
    item->name = strdup(name ? name : "");
    if (!item->name) {
        free(item);
        return 8;
    }
    item->type = type;
    item->size = size;
    if (size)
        memcpy(item->data, data, size);
    RegistryValue *old = *link;
    item->next = old ? old->next : NULL;
    *link = item;
    if (old) {
        free(old->name);
        free(old);
    }
    return 0;
}
WIN int32_t win_RegQueryValueExA(void *handle, const char *name, DWORD *reserved, DWORD *type,
                                     void *data, DWORD *size)
{
    (void)reserved;
    RegistryKey *key = reg_handle(handle);
    if (!key)
        return 6;
    if (data && !size)
        return 87;
    RegistryValue *item = *reg_value(key, name);
    if (!item)
        return 2;
    if (type)
        *type = item->type;
    DWORD capacity = size ? *size : 0;
    if (size)
        *size = item->size;
    if (data) {
        if (capacity < item->size)
            return 234;
        memcpy(data, item->data, item->size);
    }
    return 0;
}
WIN int32_t win_RegGetValueA(void *handle, const char *subkey, const char *name, DWORD flags,
                                 DWORD *type, void *data, DWORD *size)
{
    RegistryKey *key = lookup_key(handle, subkey, 0, NULL);
    DWORD actual_type = 0, capacity = size ? *size : 0;
    int32_t status = key ? win_RegQueryValueExA(key, name, NULL, &actual_type, data, size) : 2;
    if (!status && (flags & 0xffff) && ((flags & 0xffff) != 0xffff) &&
        (actual_type >= 7 || !(flags & (1u << actual_type))))
        status = 1630;
    if (status && data && (flags & 0x20000000))
        memset(data, 0, capacity);
    if (type)
        *type = actual_type;
    return status;
}
WIN int32_t win_RegDeleteValueA(void *handle, const char *name)
{
    RegistryKey *key = reg_handle(handle);
    if (!key)
        return 6;
    RegistryValue **link = reg_value(key, name);
    RegistryValue *item = *link;
    if (!item)
        return 2;
    *link = item->next;
    free(item->name);
    free(item);
    return 0;
}
