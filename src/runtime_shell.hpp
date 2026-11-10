#pragma once
inline constexpr auto runtimeShell = R"MOST(
static std::filesystem::path special_folder(uint32_t folder)
{
    using Paths = QStandardPaths;
    Paths::StandardLocation location;
    switch (folder) {
    case 0: case 0x10: location = Paths::DesktopLocation; break;
    case 2: case 0xb: case 0x17: location = Paths::ApplicationsLocation; break;
    case 5: case 0x2e: location = Paths::DocumentsLocation; break;
    case 0x1a: case 0x1c: case 0x23: location = Paths::GenericDataLocation; break;
    case 0x24: return guest_windows_folders().windows;
    case 0x25: return guest_windows_folders().system;
    case 0x28: location = Paths::HomeLocation; break;
    default: return {};
    }
    return std::filesystem::path(Paths::writableLocation(location).toStdU16String());
}

static uint32_t win_SHGetSpecialFolderLocation(uint32_t* sp)
{
    const auto window = pop32(sp), folder = pop32(sp), output = pop32(sp);
    if (!output || (window && !widget_at(window)))
        return 0x80070057u;
    store32(output, 0);
    const auto path = special_folder(folder & 0xff);
    if (path.empty())
        return 0x80070002u;
    if (folder & 0x8000) {
        std::error_code error;
        std::filesystem::create_directories(path, error);
        if (error)
            return 0x80070005u;
    }
    const auto pidl = allocate_shell_item(path);
    store32(output, pidl);
    return pidl ? 0 : 0x8007000eu;
}

static uint32_t win_SHGetPathFromIDListW(uint32_t* sp)
{
    const auto pidl = pop32(sp), output = pop32(sp);
    if (!output)
        return gui_error(87);
    const auto entry = shell_items.find(pidl);
    if (entry == shell_items.end()) {
        store_wide(output, u"", 0);
        return gui_error(87);
    }
    const auto path = windows_path(entry->second);
    store_wide(output, path.data(), path.size());
    return 1;
}

static uint32_t win_SHBrowseForFolderW(uint32_t* sp)
{
    const auto address = pop32(sp);
    if (!address)
        return gui_error(87);
    uint32_t info[8];
    memcpy(info, memory(address, sizeof info, 0), sizeof info);
    auto* owner = info[0] ? widget_at(info[0]) : nullptr;
    if ((info[0] && !owner) || (info[5] && !is_guest_callback(info[5])))
        return gui_error(87);
    const auto root = shell_items.find(info[1]);
    if (info[1] && root == shell_items.end())
        return gui_error(87);
    std::filesystem::path root_path = root == shell_items.end() ? std::filesystem::path() : root->second;
    QString title;
    if (!widget_string(info[3], title))
        return gui_error(87);
    widget_app();
    QFileDialog chooser(nullptr, title);
    chooser.setOption(QFileDialog::DontUseNativeDialog);
    chooser.setOption(QFileDialog::ShowDirsOnly);
    chooser.setFileMode(QFileDialog::Directory);
    if (!root_path.empty())
        chooser.setDirectory(QString::fromStdU16String(root_path.u16string()));
    if (guest_widgets.size() >= 4096 || next_widget == 0x72000000u)
        return gui_error(8);
    const auto handle = next_widget++;
    auto& entry = guest_widgets.emplace(handle,
        GuestWidget{&chooser, QStringLiteral("#32770"), info[0], 0, 0, 0, 0}).first->second;
    entry.destroying = true;
    struct Unregister {
        uint32_t handle;
        ~Unregister() { guest_widgets.erase(handle); }
    } cleanup{handle};
    if (info[5])
        call_guest(sp, info[5], handle, 1, 0, info[6]); // BFFM_INITIALIZED
    if (chooser.exec() != QDialog::Accepted || chooser.selectedFiles().isEmpty())
        return 0;
    std::error_code error;
    const auto selected = std::filesystem::canonical(
        std::filesystem::path(chooser.selectedFiles().first().toStdU16String()), error);
    if (error)
        return gui_error(3);
    if (!root_path.empty()) {
        root_path = std::filesystem::canonical(root_path, error);
        if (error)
            return gui_error(3);
        const auto relative = selected.lexically_relative(root_path);
        if (relative.empty() || *relative.begin() == "..")
            return gui_error(5);
    }
    const auto pidl = allocate_shell_item(selected);
    if (!pidl)
        return gui_error(8);
    if (info[2]) {
        const auto name = selected.filename().u16string();
        store_wide(info[2], name.data(), std::min<size_t>(name.size(), 259));
    }
    store32(address + 28, 0);
    return pidl;
}

static uint32_t win_SHGetFileInfoW(uint32_t* sp)
{
    const auto name = pop32(sp), attributes = pop32(sp), output = pop32(sp);
    const auto size = pop32(sp), flags = pop32(sp);
    if (!output || size < 692 || (flags & ~0xf19u))
        return gui_error(87);
    std::filesystem::path path;
    if (flags & 8) {
        const auto entry = shell_items.find(name);
        if (entry == shell_items.end())
            return gui_error(87);
        path = entry->second;
    } else {
        path = guest_path(name);
    }
    if (path.empty())
        return 0;
    const QFileInfo info(QString::fromStdU16String(path.u16string()));
    const bool specified = flags & 0x10;
    if (!specified && !info.exists())
        return gui_error(2);
    const bool directory = specified ? (attributes & 0x10) != 0 : info.isDir();
    auto* data = memory(output, 692, 1);
    memset(data, 0, 692);
    if (flags & 0x800)
        store32(output + 8, (directory ? 0x20000000u : 0) | 0x40000000u);
    if (flags & 0x200) {
        const auto text = info.fileName().left(259);
        memcpy(data + 12, text.utf16(), text.size() * 2);
    }
    if (flags & 0x400 || flags & 0x100) {
        widget_app();
        QFileIconProvider icons;
        if (flags & 0x100) {
            if (guest_images.size() >= 1024 || next_image == 0x76000000u)
                return gui_error(8);
            const auto icon = specified ? icons.icon(directory ? QFileIconProvider::Folder : QFileIconProvider::File)
                                       : icons.icon(info);
            const auto pixels = flags & 1 ? 16 : 32;
            auto image = icon.pixmap(pixels, pixels).toImage();
            if (image.isNull())
                return gui_error(2);
            const auto handle = next_image++;
            guest_images.emplace(handle, GuestImage{1, std::move(image)});
            store32(output, handle);
        }
        if (flags & 0x400) {
            const auto type = (specified ? (directory ? QStringLiteral("Folder") : QStringLiteral("File"))
                                         : icons.type(info)).left(79);
            memcpy(data + 532, type.utf16(), type.size() * 2);
        }
    }
    return 1;
}

static uint32_t win_ShellExecuteW(uint32_t* sp)
{
    const auto window = pop32(sp), verb = pop32(sp), name = pop32(sp);
    const auto parameters = pop32(sp), cwd = pop32(sp);
    pop32(sp); // Show mode does not apply to native Linux children.
    if (window && !widget_at(window))
        return 5;
    std::u16string action, args;
    if ((verb && !read_guest_wide(verb, action)) || (parameters && !read_guest_wide(parameters, args)))
        return 5;
    if (!action.empty() && fold_ascii(action) != u"OPEN")
        return 31;
    const auto path = guest_path(name);
    const auto directory = cwd ? guest_path(cwd) : std::filesystem::path();
    if (path.empty() || (cwd && directory.empty()))
        return 2;
    std::vector<std::string> arguments{path.string()};
    try {
        for (const auto& value : parse_child_arguments(args))
            arguments.push_back(std::filesystem::path(value).string());
    } catch (const std::filesystem::filesystem_error&) {
        return 31;
    }
    std::erase_if(children, [](const auto& child) {
        return refresh_child(*child, false) && child->exited;
    });
    if (children.size() >= 1024)
        return 8;
    auto child = std::make_shared<GuestChild>();
    children.push_back(child);
    const auto error = spawn_guest_elf(path, arguments, directory, child->pid);
    if (error)
        children.pop_back();
    return error ? error == ENOEXEC ? 31 : std::min(32u, launch_error(error)) : 33;
}

static uint32_t win_SHFileOperationW(uint32_t* sp)
{
    const auto address = pop32(sp);
    if (!address)
        return 87;
    const auto window = load32(address), function = load32(address + 4);
    const auto from = load32(address + 8), to = load32(address + 12);
    const auto flags = load16(address + 16);
    store32(address + 20, 0);
    store32(address + 24, 0);
    if ((window && !widget_at(window)) || !from || function < 1 || function > 4)
        return 87;
    if (!(flags & 0x10) || (flags & ~0x0614u) || (function != 3 && !to))
        return 50;
    std::u16string source, destination;
    if (!read_guest_wide(from, source) || source.empty() || load16(from + (source.size() + 1) * 2))
        return 50;
    if (function != 3 && (!read_guest_wide(to, destination) || destination.empty()
        || load16(to + (destination.size() + 1) * 2)))
        return 50;
    if (function == 3) {
        const auto path = resolve_guest_path(source);
        struct stat info{};
        if (path.empty() || lstat(path.c_str(), &info) < 0)
            return path.empty() ? last_error : file_error(errno);
        push32(sp, from);
        return (S_ISDIR(info.st_mode) ? win_RemoveDirectoryW(sp) : win_DeleteFileW(sp)) ? 0 : last_error;
    }
    if (function == 2) {
        push32(sp, 0);
        push32(sp, to);
        push32(sp, from);
        return win_CopyFileW(sp) ? 0 : last_error;
    }
    return move_path(from, to, 0) ? 0 : last_error;
}
)MOST";
