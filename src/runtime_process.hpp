#pragma once
inline constexpr auto runtimeProcess = R"MOST(
static uint32_t find_builtin_module(const std::u16string&, bool);

static uint32_t main_module(const std::u16string& name)
{
    if (name.empty()) {
        last_error = 126;
        return 0;
    }
    if (const auto handle = find_builtin_module(name, false))
        return handle;
    std::error_code error;
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error) {
        try {
            auto candidate = name;
            std::replace(candidate.begin(), candidate.end(), u'/', u'\\');
            const auto actual = candidate.find(u'\\') == candidate.npos ?
                executable.filename().u16string() : windows_path(executable);
            if (fold_ascii(candidate) == fold_ascii(actual))
                return guest_image_base;
        } catch (const std::filesystem::filesystem_error&) {}
    }
    last_error = 126;
    return 0;
}

static uint32_t win_GetModuleHandleW(uint32_t *sp)
{
    const uint32_t address = pop32(sp);
    if (!address)
        return guest_image_base;
    std::u16string name;
    return read_guest_wide(address, name) ? main_module(name) : 0;
}

static uint32_t win_GetModuleHandleA(uint32_t *sp)
{
    const uint32_t address = pop32(sp);
    if (!address)
        return guest_image_base;
    uint32_t bytes = 0;
    while (*memory(address + bytes, 1, 0)) {
        if (++bytes == 32767) {
            last_error = 206;
            return 0;
        }
    }
    std::string utf16;
    bool replaced = false;
    if (!transcode_guest("CP1252", "UTF-16LE", memory(address, bytes, 0), bytes,
                         "\xfd\xff", 2, 1, false, utf16, replaced))
        return 0;
    std::u16string name(utf16.size() / 2, u'\0');
    memcpy(name.data(), utf16.data(), utf16.size());
    return main_module(name);
}

static void append_windows_argument(std::u16string& line, std::u16string_view value)
{
    line += u'"';
    size_t slashes = 0;
    for (const auto unit : value) {
        if (unit == u'\\') {
            ++slashes;
            continue;
        }
        line.append(unit == u'"' ? slashes * 2 + 1 : slashes, u'\\');
        slashes = 0;
        line += unit;
    }
    line.append(slashes * 2, u'\\');
    line += u'"';
}

static uint32_t win_GetCommandLineW(uint32_t *)
{
    if (!command_line.empty())
        return command_line_address;
    std::error_code error;
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
    if (error) {
        last_error = 2;
        return 0;
    }
    try {
        std::u16string line;
        append_windows_argument(line, windows_path(executable));
        for (int i = 1; i < host_argc; ++i) {
            line += u' ';
            append_windows_argument(line, std::filesystem::path(host_argv[i]).u16string());
        }
        if (line.size() >= 32767) {
            last_error = 206;
            return 0;
        }
        command_line = std::move(line);
        command_line += u'\0';
        return command_line_address;
    } catch (const std::filesystem::filesystem_error&) {
        last_error = 123;
        return 0;
    }
}

static uint32_t win_GetTempFileNameW(uint32_t *sp)
{
    const uint32_t directory = pop32(sp), prefix_address = pop32(sp);
    const uint32_t unique = pop32(sp), output = pop32(sp);
    const auto path = guest_path(directory);
    std::u16string prefix;
    if (path.empty() || !output || !read_guest_wide(prefix_address, prefix))
        return 0;
    prefix.resize(std::min<size_t>(prefix.size(), 3));
    if (prefix.find_first_of(u"\\/:*?<>|\"") != prefix.npos) {
        last_error = 123;
        return 0;
    }
    try {
        uint32_t id = unique ? (unique & 0xffff) : win_GetTickCount(nullptr) % 65535 + 1;
        for (unsigned attempt = 0; attempt < 65535; ++attempt, id = id % 65535 + 1) {
            char suffix[9];
            snprintf(suffix, sizeof suffix, "%04X.TMP", unsigned(id));
            auto name = prefix;
            for (const char* cursor = suffix; *cursor; ++cursor)
                name += char16_t(*cursor);
            const auto candidate = path / name;
            const auto wide = windows_path(candidate);
            if (wide.size() >= 260) {
                last_error = 111;
                return 0;
            }
            memory(output, (wide.size() + 1) * 2, 1);
            if (!unique) {
                const int fd = open(candidate.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
                if (fd < 0) {
                    if (errno == EEXIST)
                        continue;
                    last_error = file_error(errno);
                    return 0;
                }
                if (close(fd) != 0) {
                    last_error = file_error(errno);
                    unlink(candidate.c_str());
                    return 0;
                }
            }
            store_wide(output, wide.data(), wide.size());
            return unique ? unique : id;
        }
        last_error = 80;
        return 0;
    } catch (const std::filesystem::filesystem_error&) {
        last_error = 123;
        return 0;
    }
}

static uint32_t win_GetShortPathNameW(uint32_t *sp)
{
    const uint32_t input = pop32(sp), output = pop32(sp), capacity = pop32(sp);
    const auto path = guest_path(input);
    if (path.empty())
        return 0;
    struct stat info;
    if (stat(path.c_str(), &info) != 0) {
        last_error = file_error(errno);
        return 0;
    }
    try {
        const auto wide = windows_path(path);
        if (capacity <= wide.size())
            return wide.size() + 1;
        if (!output) {
            last_error = 87;
            return 0;
        }
        store_wide(output, wide.data(), wide.size());
        return wide.size();
    } catch (const std::filesystem::filesystem_error&) {
        last_error = 123;
        return 0;
    }
}
static void append_search_directories(std::vector<std::filesystem::path>& directories,
                                      std::u16string_view list)
{
    size_t start = 0;
    while (start < list.size()) {
        const auto end = list.find(u';', start);
        const auto item = list.substr(start, end - start);
        if (!item.empty()) {
            auto directory = resolve_guest_path(std::u16string(item));
            if (!directory.empty())
                directories.push_back(std::move(directory));
        }
        if (end == list.npos)
            break;
        start = end + 1;
    }
}

static uint32_t win_SearchPathW(uint32_t *sp)
{
    const uint32_t paths = pop32(sp), file = pop32(sp), extension = pop32(sp);
    const uint32_t capacity = pop32(sp), output = pop32(sp), part = pop32(sp);
    std::u16string name, suffix, list;
    if (!read_guest_wide(file, name) || name.empty() ||
        (extension && !read_guest_wide(extension, suffix)) || (paths && !read_guest_wide(paths, list))) {
        last_error = 87;
        return 0;
    }
    const auto separator = name.find_last_of(u"\\/");
    if (name.find(u'.', separator == name.npos ? 0 : separator + 1) == name.npos)
        name += suffix;
    auto resolved = resolve_guest_path(name);
    if (resolved.empty())
        return 0;

    std::vector<std::filesystem::path> directories;
    if (separator != name.npos || (name.size() >= 2 && name[1] == u':')) {
        directories.push_back(resolved.parent_path());
    } else if (paths) {
        append_search_directories(directories, list);
    } else {
        std::error_code error;
        const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
        if (!error)
            directories.push_back(executable.parent_path());
        directories.push_back(resolved.parent_path());
        if (const auto env = guest_environment.find(u"PATH"); env != guest_environment.end())
            append_search_directories(directories, env->second);
    }
    for (const auto& directory : directories) {
        const auto candidate = directory / resolved.filename();
        struct stat info;
        if (stat(candidate.c_str(), &info) != 0 || !S_ISREG(info.st_mode))
            continue;
        try {
            const auto wide = windows_path(candidate);
            if (capacity <= wide.size())
                return wide.size() + 1;
            if (!output) {
                last_error = 87;
                return 0;
            }
            store_wide(output, wide.data(), wide.size());
            if (part)
                store32(part, output + (wide.find_last_of(u'\\') + 1) * 2);
            return wide.size();
        } catch (const std::filesystem::filesystem_error&) {
            last_error = 123;
            return 0;
        }
    }
    last_error = 2;
    return 0;
}
)MOST";
