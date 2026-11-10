#pragma once
inline constexpr auto runtimeCom = R"MOST(
static constexpr uint32_t com_method_base = 0xf3000000u;
static constexpr uint32_t com_invalid = 0x80070057u, com_no_interface = 0x80004002u;
static constexpr uint32_t com_not_implemented = 0x80004001u;

struct ShellLink {
    uint32_t object = 0, refs = 1;
    std::u16string path, args, description, directory, icon;
    uint32_t icon_index = 0, show = 1, hotkey = 0;
};
static std::map<uint32_t, ShellLink> shell_links;

using Guid = std::array<uint32_t, 4>;
static constexpr Guid clsid_shell_link{0x00021401u, 0, 0x000000c0u, 0x46000000u};
static constexpr Guid iid_shell_link{0x000214f9u, 0, 0x000000c0u, 0x46000000u};
static constexpr Guid iid_persist_file{0x0000010bu, 0, 0x000000c0u, 0x46000000u};
static constexpr Guid iid_unknown{0, 0, 0x000000c0u, 0x46000000u};

static bool guid_matches(uint32_t address, const Guid& guid)
{
    return address && !memcmp(memory(address, sizeof guid, 0), guid.data(), sizeof guid);
}

static bool query_shell_interface(ShellLink& link, uint32_t iid, uint32_t output)
{
    uint32_t iface = 0;
    if (guid_matches(iid, iid_shell_link) || guid_matches(iid, iid_unknown))
        iface = link.object;
    else if (guid_matches(iid, iid_persist_file))
        iface = link.object + 4;
    store32(output, iface);
    return iface != 0;
}

static uint32_t win_CoCreateInstance(uint32_t* sp)
{
    const auto clsid = pop32(sp), outer = pop32(sp), context = pop32(sp);
    const auto iid = pop32(sp), output = pop32(sp);
    if (!output || !clsid || !iid)
        return com_invalid;
    store32(output, 0);
    if (outer)
        return 0x80040110u;
    if (!ole_depth)
        return 0x800401f0u;
    if (!guid_matches(clsid, clsid_shell_link) || !(context & 1))
        return 0x80040154u;
    if (shell_links.size() >= 4096)
        return 0x8007000eu;
    std::vector<uint8_t> bytes((2 + 21 + 9) * 4);
    const auto address = allocate_guest_blob(std::move(bytes));
    if (!address)
        return 0x8007000eu;
    const auto table = address + 8, persist = table + 21 * 4;
    store32(address, table);
    store32(address + 4, persist);
    for (uint32_t i = 0; i < 21; ++i)
        store32(table + i * 4, com_method_base + i);
    for (uint32_t i = 0; i < 9; ++i)
        store32(persist + i * 4, com_method_base + 0x20 + i);
    ShellLink link;
    link.object = address;
    if (!query_shell_interface(link, iid, output)) {
        free_guest_blob(address);
        return com_no_interface;
    }
    shell_links.emplace(address, std::move(link));
    return 0;
}

static QString desktop_escape(QString value)
{
    value.replace(u'\\', QStringLiteral("\\\\"));
    value.replace(u'\n', QStringLiteral("\\n"));
    value.replace(u'\r', QStringLiteral("\\r"));
    value.replace(u'\t', QStringLiteral("\\t"));
    if (value.startsWith(u' '))
        value.replace(0, 1, QStringLiteral("\\s"));
    if (value.endsWith(u' '))
        value.replace(value.size() - 1, 1, QStringLiteral("\\s"));
    return value;
}

static QString desktop_argument(QString value)
{
    value.replace(u'%', QStringLiteral("%%"));
    QString quoted = QStringLiteral("\"");
    for (const auto unit : value) {
        if (unit == u'"' || unit == u'`' || unit == u'$' || unit == u'\\')
            quoted += u'\\';
        quoted += unit;
    }
    quoted += u'"';
    return desktop_escape(quoted);
}

static uint32_t save_shell_link(const ShellLink& link, uint32_t destination)
{
    auto output = guest_path(destination);
    if (output.empty() || link.path.empty())
        return com_invalid;
    if (fold_ascii(output.extension().u16string()) == u".LNK")
        output.replace_extension(".desktop");
    if (output.extension() != ".desktop")
        return com_invalid;
    auto target = resolve_guest_path(link.path);
    if (target.empty() || target.string().find_first_of("=\r\n") != std::string::npos)
        return com_invalid;
    auto path = QString::fromStdU16String(target.u16string());
    if (access(target.c_str(), X_OK) != 0)
        return 0x80070002u;
    const int fd = open(target.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return 0x80070005u;
    char magic[4];
    const auto count = read(fd, magic, sizeof magic);
    close(fd);
    if (count != sizeof magic || memcmp(magic, "\x7f" "ELF", sizeof magic))
        return 0x800700c1u;
    QString exec = desktop_argument(path);
    for (const auto& value : parse_child_arguments(link.args)) {
        if (value.find_first_of(u"\r\n") != value.npos)
            return com_invalid;
        exec += u' ';
        exec += desktop_argument(QString::fromStdU16String(value));
    }
    const auto name = desktop_escape(QString::fromStdU16String(output.stem().u16string()));
    QString entry = QStringLiteral("[Desktop Entry]\nType=Application\nName=") + name
        + QStringLiteral("\nExec=") + exec + QStringLiteral("\nTerminal=false\n");
    if (!link.description.empty())
        entry += QStringLiteral("Comment=") + desktop_escape(QString::fromStdU16String(link.description)) + u'\n';
    if (!link.directory.empty()) {
        const auto directory = resolve_guest_path(link.directory);
        if (directory.empty())
            return com_invalid;
        entry += QStringLiteral("Path=") + desktop_escape(QString::fromStdU16String(directory.u16string())) + u'\n';
    }
    if (!link.icon.empty()) {
        const auto icon = resolve_guest_path(link.icon);
        if (!icon.empty() && (icon.extension() == ".png" || icon.extension() == ".svg" || icon.extension() == ".xpm"))
            entry += QStringLiteral("Icon=") + desktop_escape(QString::fromStdU16String(icon.u16string())) + u'\n';
    }
    QSaveFile file(QString::fromStdU16String(output.u16string()));
    const auto encoded = entry.toUtf8();
    if (!file.open(QIODevice::WriteOnly) || file.write(encoded) != encoded.size() || !file.commit())
        return 0x80070005u;
    return 0;
}

static uint32_t call_com_method(uint32_t method, uint32_t* sp)
{
    const auto iface = pop32(sp);
    const bool persist = method >= 0x20;
    const auto link = shell_links.find(persist ? iface - 4 : iface);
    if (link == shell_links.end())
        guest_fault();
    auto& state = link->second;
    if (persist)
        method -= 0x20;
    switch (method) {
    case 0: {
        const auto iid = pop32(sp), output = pop32(sp);
        if (!iid || !output)
            return com_invalid;
        if (!query_shell_interface(state, iid, output))
            return com_no_interface;
        ++state.refs;
        return 0;
    }
    case 1: return ++state.refs;
    case 2: {
        const auto remaining = --state.refs;
        if (!remaining) {
            free_guest_blob(state.object);
            shell_links.erase(link);
        }
        return remaining;
    }
    default: break;
    }
    if (persist) {
        switch (method) {
        case 3: {
            const auto output = pop32(sp);
            if (!output)
                return com_invalid;
            memcpy(memory(output, sizeof clsid_shell_link, 1), clsid_shell_link.data(), sizeof clsid_shell_link);
            return 0;
        }
        case 4: return 0;
        case 5: pop32(sp); pop32(sp); return com_not_implemented;
        case 6: {
            const auto output = pop32(sp);
            pop32(sp); // fRemember
            return save_shell_link(state, output);
        }
        case 7: pop32(sp); return com_not_implemented;
        case 8: {
            const auto output = pop32(sp);
            if (output)
                store32(output, 0);
            return com_not_implemented;
        }
        default: guest_fault();
        }
    }
    switch (method) {
    case 7: case 9: case 11: case 20: {
        const auto address = pop32(sp);
        auto* field = method == 7 ? &state.description : method == 9 ? &state.directory :
            method == 11 ? &state.args : &state.path;
        std::u16string value;
        if (!read_guest_wide(address, value))
            return com_invalid;
        *field = std::move(value);
        return 0;
    }
    case 13: state.hotkey = pop32(sp) & 0xffff; return 0;
    case 15: state.show = pop32(sp); return 0;
    case 17: {
        const auto address = pop32(sp), index = pop32(sp);
        std::u16string icon;
        if (!read_guest_wide(address, icon))
            return com_invalid;
        state.icon = std::move(icon);
        state.icon_index = index;
        return 0;
    }
    // Preserve stack balance for the remaining IShellLinkW slots.
    case 3: for (int i = 0; i < 4; ++i) pop32(sp); return com_not_implemented;
    case 4: case 5: case 12: case 14: pop32(sp); return com_not_implemented;
    case 6: case 8: case 10: case 18: case 19: pop32(sp); pop32(sp); return com_not_implemented;
    case 16: pop32(sp); pop32(sp); pop32(sp); return com_not_implemented;
    default: guest_fault();
    }
}
)MOST";
