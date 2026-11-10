#pragma once
inline constexpr auto runtimeModuleDeclarations = R"MOST(
struct BuiltinExport {
    const char* dll;
    const char* name;
    uint32_t (*call)(uint32_t*);
};
static uint32_t win_LoadLibraryExW(uint32_t*);
static uint32_t win_GetProcAddress(uint32_t*);
static uint32_t win_FreeLibrary(uint32_t*);
)MOST";

inline constexpr auto runtimeModules = R"MOST(
static constexpr uint32_t builtin_module_base = 0xf1000000u, builtin_proc_base = 0xf2000000u;
struct BuiltinModule {
    const char* name;
    unsigned references = 0;
    bool pinned = false;
};
static BuiltinModule builtin_modules[]{
    {"kernel32.dll", 0, true}, {"user32.dll"}, {"gdi32.dll"}, {"advapi32.dll"},
    {"shell32.dll"}, {"comctl32.dll"}, {"ole32.dll"}
};

static void pin_builtin_module(std::string_view name)
{
    for (auto& module : builtin_modules)
        if (name == module.name)
            module.pinned = true;
}

static BuiltinModule* builtin_module(uint32_t handle)
{
    if (handle < builtin_module_base || handle - builtin_module_base >= std::size(builtin_modules))
        return nullptr;
    auto& module = builtin_modules[handle - builtin_module_base];
    return module.pinned || module.references ? &module : nullptr;
}

static uint32_t find_builtin_module(const std::u16string& name, bool load)
{
    if (name.find_first_of(u"/\\:") != name.npos)
        return 0;
    auto normalized = fold_ascii(name);
    if (normalized.find(u'.') == normalized.npos)
        normalized += u".DLL";
    for (unsigned i = 0; i < std::size(builtin_modules); ++i) {
        auto& module = builtin_modules[i];
        std::u16string expected;
        for (const char* p = module.name; *p; ++p)
            expected += char16_t(*p);
        if (normalized != fold_ascii(expected))
            continue;
        if (load) {
            const auto has_exports = std::any_of(std::begin(builtin_exports), std::end(builtin_exports),
                [&](const BuiltinExport& entry) { return !strcmp(entry.dll, module.name); });
            if (!has_exports || module.references == UINT32_MAX)
                return 0;
            ++module.references;
        }
        return builtin_module(builtin_module_base + i) ? builtin_module_base + i : 0;
    }
    return 0;
}

static uint32_t win_LoadLibraryExW(uint32_t* sp)
{
    const auto address = pop32(sp), file = pop32(sp), flags = pop32(sp);
    if (file || (flags & ~0x1e00u))
        return process_error(50);
    std::u16string name;
    if (!read_guest_wide(address, name))
        return 0;
    const auto handle = find_builtin_module(name, true);
    return handle ? handle : process_error(126);
}

static uint32_t win_FreeLibrary(uint32_t* sp)
{
    auto* module = builtin_module(pop32(sp));
    if (!module)
        return process_error(6);
    if (module->references)
        --module->references;
    return 1;
}

static uint32_t builtin_address(std::string_view dll, std::string_view name)
{
    for (unsigned i = 0; i < std::size(builtin_exports); ++i)
        if (dll == builtin_exports[i].dll && name == builtin_exports[i].name)
            return builtin_proc_base + i;
    return 0;
}

static uint32_t win_GetProcAddress(uint32_t* sp)
{
    const auto handle = pop32(sp), address = pop32(sp);
    const auto* module = builtin_module(handle);
    if (!module || !address)
        return process_error(module ? 87 : 6);
    std::string name;
    if (address <= 0xffff) {
        name = "#" + std::to_string(address);
    } else {
        for (unsigned i = 0; i < 256; ++i) {
            const auto value = *memory(address + i, 1, 0);
            if (!value)
                break;
            name += char(value);
        }
    }
    const auto entry = builtin_address(module->name, name);
    return entry ? entry : process_error(127);
}

static uint32_t call_indirect(uint32_t address, uint32_t* sp)
{
#ifdef MOST_WIDGETS
    if (address >= com_method_base && address < com_method_base + 0x100)
        return call_com_method(address - com_method_base, sp);
#endif
    if (address < builtin_proc_base || address - builtin_proc_base >= std::size(builtin_exports))
        guest_fault();
    const auto& entry = builtin_exports[address - builtin_proc_base];
    const auto module = std::find_if(std::begin(builtin_modules), std::end(builtin_modules),
        [&](const BuiltinModule& item) { return !strcmp(entry.dll, item.name); });
    if (module == std::end(builtin_modules) || (!module->pinned && !module->references))
        guest_fault();
    trace_guest_call(entry.name);
    return entry.call(sp);
}
)MOST";
