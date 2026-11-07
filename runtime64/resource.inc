static _Thread_local uint16_t ui_language = 0x409;
static _Thread_local DWORD thread_locale = 0x409;
WIN uint16_t win_GetUserDefaultUILanguage(void) { return 0x409; }
WIN uint16_t win_GetThreadUILanguage(void) { return ui_language; }
WIN uint16_t win_SetThreadUILanguage(uint16_t language)
{
    ui_language = language ? language : 0x409;
    return ui_language;
}
WIN DWORD win_GetThreadLocale(void) { return thread_locale; }
WIN int win_SetThreadLocale(DWORD locale) { thread_locale = locale; return 1; }

static const unsigned char *resource_at(size_t offset, size_t bytes)
{
    if (offset > GUEST_RSRC_SIZE || bytes > GUEST_RSRC_SIZE - offset)
        return NULL;
    return (const unsigned char *)GUEST_RSRC + offset;
}

static uint16_t read_word(const void *ptr)
{
    uint16_t result;
    memcpy(&result, ptr, sizeof(result));
    return result;
}

static DWORD read_dword(const void *ptr)
{
    DWORD result;
    memcpy(&result, ptr, sizeof(result));
    return result;
}

static int resource_matches(DWORD entry, const char *name)
{
    if ((uintptr_t)name <= 0xffff)
        return entry == (uintptr_t)name;
    if (!(entry & 0x80000000))
        return 0;
    const unsigned char *prefix = resource_at(entry & 0x7fffffff, 2);
    if (!prefix)
        return 0;
    size_t length = read_word(prefix);
    const unsigned char *text = resource_at((entry & 0x7fffffff) + 2, length * 2);
    if (!text || strlen(name) != length)
        return 0;
    for (size_t i = 0; i < length; ++i)
        if (read_word(text + i * 2) != (unsigned char)name[i])
            return 0;
    return 1;
}

static DWORD resource_child(DWORD offset, const char *name, int allow_fallback)
{
    const unsigned char *directory = resource_at(offset, 16);
    if (!directory)
        return 0;
    size_t count = read_word(directory + 12) + read_word(directory + 14);
    const unsigned char *entries = resource_at(offset + 16, count * 8);
    if (!entries)
        return 0;
    for (size_t i = 0; i < count; ++i)
        if (resource_matches(read_dword(entries + i * 8), name))
            return read_dword(entries + i * 8 + 4);
    return allow_fallback && count ? read_dword(entries + 4) : 0;
}

WIN const void *win_FindResourceA(void *module, const char *name, const char *type)
{
    if (module && module != (void *)GUEST_BASE) {
        last_error = 6;
        return NULL;
    }
    DWORD type_dir = resource_child(0, type, 0);
    if (type_dir & 0x80000000) {
        DWORD name_dir = resource_child(type_dir & 0x7fffffff, name, 0);
        if (name_dir & 0x80000000) {
            DWORD leaf = resource_child(name_dir & 0x7fffffff,
                                        (const char *)(uintptr_t)ui_language, 1);
            if (leaf && !(leaf & 0x80000000))
                return resource_at(leaf, 16);
        }
    }
    last_error = 1814;
    return NULL;
}
WIN DWORD win_SizeofResource(void *module, const unsigned char *resource)
{
    if ((module && module != (void *)GUEST_BASE) || !resource)
        return 0;
    return read_dword(resource + 4);
}
WIN const void *win_LoadResource(void *module, const unsigned char *resource)
{
    if ((module && module != (void *)GUEST_BASE) || !resource)
        return NULL;
    DWORD rva = read_dword(resource);
    DWORD size = read_dword(resource + 4);
    if (rva >= GUEST_SIZE || size > GUEST_SIZE - rva)
        return NULL;
    return (const void *)(GUEST_BASE + rva);
}
WIN const void *win_LockResource(const void *resource) { return resource; }
