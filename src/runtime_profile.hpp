#pragma once
inline constexpr auto runtimeProfile = R"MOST(
struct GuestWindowsFolders {
    std::filesystem::path root, windows, system;

    GuestWindowsFolders()
    {
        char location[] = "/tmp/most-pe32-XXXXXX";
        if (!mkdtemp(location))
            return;
        root = location;
        const auto directory = root / "Windows";
        const auto system_directory = directory / "System32";
        std::error_code error;
        std::filesystem::create_directories(system_directory, error);
        if (!error) {
            windows = directory;
            system = system_directory;
        }
    }

    ~GuestWindowsFolders()
    {
        if (root.empty())
            return;
        std::error_code error;
        std::filesystem::remove(root / "Windows" / "System32", error);
        std::filesystem::remove(root / "Windows", error);
        std::filesystem::remove(root, error);
    }
};

static const GuestWindowsFolders& guest_windows_folders()
{
    static const GuestWindowsFolders folders;
    return folders;
}

static uint32_t copy_windows_directory(uint32_t *sp, const std::filesystem::path& path)
{
    const uint32_t output = pop32(sp), capacity = pop32(sp);
    if (path.empty()) {
        last_error = 3;
        return 0;
    }
    const auto wide = windows_path(path);
    if (capacity <= wide.size())
        return wide.size() + 1;
    if (!output) {
        last_error = 87;
        return 0;
    }
    store_wide(output, wide.data(), wide.size());
    return wide.size();
}

static uint32_t win_GetWindowsDirectoryW(uint32_t *sp)
{
    return copy_windows_directory(sp, guest_windows_folders().windows);
}

static uint32_t win_GetSystemDirectoryW(uint32_t *sp)
{
    return copy_windows_directory(sp, guest_windows_folders().system);
}

static std::filesystem::path profile_path(uint32_t address)
{
    std::u16string name;
    if (address && !read_guest_wide(address, name))
        return {};
    if (name.empty())
        name = u"win.ini";
    if (name.front() != u'\\' && name.front() != u'/' && (name.size() < 2 || name[1] != u':')) {
        const auto& directory = guest_windows_folders().windows;
        if (directory.empty()) {
            last_error = 3;
            return {};
        }
        name = windows_path(directory) + u'\\' + name;
    }
    return resolve_guest_path(std::move(name));
}

static std::u16string_view trim_profile(std::u16string_view value)
{
    const auto first = value.find_first_not_of(u" \t\r");
    if (first == value.npos)
        return {};
    return value.substr(first, value.find_last_not_of(u" \t\r") - first + 1);
}

static std::u16string_view profile_section(std::u16string_view line)
{
    line = trim_profile(line);
    if (line.empty() || line.front() != u'[')
        return {};
    const auto end = line.find(u']');
    return end == line.npos ? std::u16string_view{} : trim_profile(line.substr(1, end - 1));
}

static std::u16string_view profile_key(std::u16string_view line)
{
    line = trim_profile(line);
    if (line.empty() || line.front() == u';' || line.front() == u'#')
        return {};
    const auto end = line.find(u'=');
    return end == line.npos ? std::u16string_view{} : trim_profile(line.substr(0, end));
}

struct ProfileFile {
    std::vector<std::u16string> lines;
    struct stat identity{};
    bool exists = false;
};

static uint32_t read_profile(const std::filesystem::path& path, ProfileFile& file)
{
    const int fd = open_existing_file(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return errno == ENOENT ? 0 : file_error(errno);
    if (fstat(fd, &file.identity) != 0 || file.identity.st_size < 0 || file.identity.st_size > 1048576) {
        close(fd);
        return 50;
    }
    file.exists = true;
    std::string bytes(file.identity.st_size, '\0');
    size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = read(fd, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            const uint32_t error = count < 0 ? file_error(errno) : 30;
            close(fd);
            return error;
        }
        offset += count;
    }
    close(fd);
    std::string utf16;
    if (bytes.starts_with("\xff\xfe")) {
        if (bytes.size() % 2)
            return 1113;
        utf16 = bytes.substr(2);
    } else {
        bool replaced = false;
        if (!transcode_guest("CP1252", "UTF-16LE", reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(),
                             "\xfd\xff", 2, 1, false, utf16, replaced))
            return last_error;
    }
    std::u16string text(utf16.size() / 2, u'\0');
    memcpy(text.data(), utf16.data(), utf16.size());
    if (text.find(u'\0') != text.npos)
        return 1113;
    size_t start = 0;
    while (start < text.size()) {
        const auto end = text.find(u'\n', start);
        auto line = text.substr(start, end - start);
        if (!line.empty() && line.back() == u'\r')
            line.pop_back();
        file.lines.push_back(std::move(line));
        if (end == text.npos)
            break;
        start = end + 1;
    }
    return 0;
}

static uint32_t win_GetPrivateProfileStringW(uint32_t *sp)
{
    const uint32_t section = pop32(sp), key = pop32(sp), fallback = pop32(sp);
    const uint32_t output = pop32(sp), capacity = pop32(sp), name = pop32(sp);
    if (!output || !capacity)
        return 0;
    std::u16string section_name, key_name, result;
    if ((section && !read_guest_wide(section, section_name)) ||
        (key && !read_guest_wide(key, key_name)) ||
        (fallback && !read_guest_wide(fallback, result)))
        return 0;
    const auto path = profile_path(name);
    if (path.empty())
        return 0;
    ProfileFile file;
    if (const auto status = read_profile(path, file)) {
        last_error = status;
        return 0;
    }
    if (!file.exists)
        last_error = 2;
    const auto wanted_section = fold_ascii(section_name), wanted_key = fold_ascii(key_name);
    const bool enumerate = !section || !key;
    if (enumerate)
        result.clear();
    else
        while (!result.empty() && result.back() == u' ')
            result.pop_back();
    bool selected = false;
    for (const auto& line : file.lines) {
        const auto header = profile_section(line);
        if (!header.empty()) {
            selected = fold_ascii(std::u16string(header)) == wanted_section;
            if (!section) {
                result += header;
                result += u'\0';
            }
            continue;
        }
        if (!section || !selected)
            continue;
        const auto item = profile_key(line);
        if (item.empty())
            continue;
        if (!key) {
            result += item;
            result += u'\0';
        } else if (fold_ascii(std::u16string(item)) == wanted_key) {
            auto value = trim_profile(std::u16string_view(line).substr(line.find(u'=') + 1));
            if (value.size() >= 2 && (value.front() == u'"' || value.front() == u'\'') &&
                value.back() == value.front()) {
                value.remove_prefix(1);
                value.remove_suffix(1);
            }
            result = value;
            break;
        }
    }
    if (!enumerate) {
        const auto count = std::min<size_t>(result.size(), capacity - 1);
        store_wide(output, result.data(), count);
        return count;
    }
    if (capacity == 1) {
        store_wide(output, u"", 0);
        return 0;
    }
    if (result.empty()) {
        store_wide(output, u"\0", 1);
        return 0;
    }
    if (result.size() < capacity) {
        store_wide(output, result.data(), result.size());
        return result.size();
    }
    result.resize(capacity - 2);
    result += u'\0';
    store_wide(output, result.data(), result.size());
    return capacity - 2;
}

static uint32_t publish_profile(const std::filesystem::path& path, const ProfileFile& file)
{
    if (file.exists && (!(file.identity.st_mode & 0222) || file.identity.st_nlink != 1))
        return 5;
    if (file.exists && share_conflict(file.identity, 6, 7))
        return 32;
    if (file.exists && faccessat(AT_FDCWD, path.c_str(), W_OK, AT_EACCESS) != 0)
        return file_error(errno);
    std::u16string contents(1, u'\ufeff');
    for (const auto& line : file.lines) {
        contents += line;
        contents += u"\r\n";
    }
    if (contents.size() > 524288)
        return 50;
    auto temporary = (path.parent_path() / ".most-ini-XXXXXX").native();
    const int fd = mkostemp(temporary.data(), O_CLOEXEC);
    if (fd < 0)
        return file_error(errno);
    uint32_t error = 0;
    size_t offset = 0;
    const size_t length = contents.size() * 2;
    while (offset < length) {
        const ssize_t written = write(fd, reinterpret_cast<const char*>(contents.data()) + offset,
                                      length - offset);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0) {
            error = written < 0 ? file_error(errno) : 29;
            break;
        }
        offset += written;
    }
    if (!error && file.exists && fchmod(fd, file.identity.st_mode & 0777) != 0)
        error = file_error(errno);
    if (close(fd) != 0 && !error)
        error = file_error(errno);
    if (!error && file.exists) {
        struct stat current;
        if (lstat(path.c_str(), &current) != 0 ||
            current.st_dev != file.identity.st_dev || current.st_ino != file.identity.st_ino ||
            current.st_ctim.tv_sec != file.identity.st_ctim.tv_sec ||
            current.st_ctim.tv_nsec != file.identity.st_ctim.tv_nsec)
            error = 32;
    }
    if (!error && renameat2(AT_FDCWD, temporary.c_str(), AT_FDCWD, path.c_str(),
                            file.exists ? 0 : RENAME_NOREPLACE) == 0)
        return 0;
    if (!error)
        error = file_error(errno);
    unlink(temporary.c_str());
    return error;
}

static uint32_t win_WritePrivateProfileStringW(uint32_t *sp)
{
    const uint32_t section = pop32(sp), key = pop32(sp), value = pop32(sp), name = pop32(sp);
    if (!section && !key && !value)
        return 1;
    std::u16string section_name, key_name, data;
    if (!read_guest_wide(section, section_name) || section_name.empty() ||
        (key && (!read_guest_wide(key, key_name) || key_name.empty())) ||
        (value && !read_guest_wide(value, data)) ||
        section_name.find_first_of(u"[]\r\n") != section_name.npos ||
        key_name.find_first_of(u"=\r\n") != key_name.npos || data.find_first_of(u"\r\n") != data.npos) {
        last_error = 87;
        return 0;
    }
    const auto path = profile_path(name);
    if (path.empty())
        return 0;
    ProfileFile file;
    if (const auto status = read_profile(path, file)) {
        last_error = status;
        return 0;
    }
    const auto wanted_section = fold_ascii(section_name), wanted_key = fold_ascii(key_name);
    auto start = std::find_if(file.lines.begin(), file.lines.end(), [&](const auto& line) {
        return fold_ascii(std::u16string(profile_section(line))) == wanted_section;
    });
    if (start == file.lines.end()) {
        if (!key || !value)
            return 1;
        file.lines.push_back(u"[" + section_name + u"]");
        file.lines.push_back(key_name + u"=" + data);
    } else {
        const auto end = std::find_if(std::next(start), file.lines.end(), [](const auto& line) {
            return !profile_section(line).empty();
        });
        if (!key) {
            file.lines.erase(start, end);
        } else {
            const auto item = std::find_if(std::next(start), end, [&](const auto& line) {
                return fold_ascii(std::u16string(profile_key(line))) == wanted_key;
            });
            if (item == end) {
                if (!value)
                    return 1;
                file.lines.insert(end, key_name + u"=" + data);
            } else if (!value) {
                file.lines.erase(item);
            } else {
                *item = key_name + u"=" + data;
            }
        }
    }
    if (const auto status = publish_profile(path, file)) {
        last_error = status;
        return 0;
    }
    return 1;
}
)MOST";
