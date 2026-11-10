#pragma once
inline constexpr auto runtimeFind = R"MOST(
struct GuestSearch {
    DIR* directory;
    std::u16string pattern;
};

static std::map<uint32_t, GuestSearch> searches;
static uint32_t next_search = 0x30000000;

static bool wildcard_match(std::u16string_view pattern, std::u16string_view name)
{
    size_t p = 0, n = 0, star = pattern.npos, after_star = 0;
    while (n < name.size()) {
        if (p < pattern.size() && pattern[p] == u'*') {
            star = p++;
            after_star = n;
        } else if (p < pattern.size() && (pattern[p] == u'?' || pattern[p] == name[n])) {
            ++p;
            ++n;
        } else if (star != pattern.npos) {
            p = star + 1;
            n = ++after_star;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == u'*')
        ++p;
    return p == pattern.size();
}

static uint64_t windows_file_time(timespec time)
{
    constexpr int64_t epoch = 11644473600LL;
    if (time.tv_sec < -epoch || time.tv_sec > int64_t(UINT64_MAX / 10000000) - epoch - 1)
        return 0;
    return uint64_t(time.tv_sec + epoch) * 10000000 + time.tv_nsec / 100;
}

struct FindDataW {
    uint32_t attributes, creation[2], access[2], write[2], size_high, size_low, reserved[2];
    char16_t name[260], alternate[14];
};
static_assert(sizeof(FindDataW) == 592);

static bool next_file(GuestSearch& search, uint32_t output)
{
    for (;;) {
        errno = 0;
        const auto* entry = readdir(search.directory);
        if (!entry) {
            last_error = errno ? file_error(errno) : 18;
            return false;
        }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        std::u16string name;
        try {
            name = std::filesystem::path(entry->d_name).u16string();
        } catch (const std::filesystem::filesystem_error&) {
            continue;
        }
        if (name.size() >= 260 || !wildcard_match(search.pattern, fold_ascii(name)))
            continue;
        struct stat info;
        if (fstatat(dirfd(search.directory), entry->d_name, &info, AT_SYMLINK_NOFOLLOW) != 0 ||
            (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode)))
            continue;
        const auto access = windows_file_time(info.st_atim), write_time = windows_file_time(info.st_mtim);
        const uint64_t size = S_ISREG(info.st_mode) ? info.st_size : 0;
        FindDataW data{};
        data.attributes = S_ISDIR(info.st_mode) ? 0x10 : (info.st_mode & 0222 ? 0x80 : 1);
        data.access[0] = access;
        data.access[1] = access >> 32;
        data.write[0] = write_time;
        data.write[1] = write_time >> 32;
        data.size_low = size;
        data.size_high = size >> 32;
        std::copy(name.begin(), name.end(), data.name);
        memcpy(memory(output, sizeof data, 1), &data, sizeof data);
        return true;
    }
}

static uint32_t win_FindFirstFileW(uint32_t *sp)
{
    const uint32_t input = pop32(sp), output = pop32(sp);
    std::u16string expression;
    if (!output || !read_guest_wide(input, expression)) {
        last_error = 87;
        return UINT32_MAX;
    }
    const auto separator = expression.find_last_of(u"\\/");
    const size_t start = separator == expression.npos ?
        (expression.size() >= 2 && expression[1] == u':' ? 2 : 0) : separator + 1;
    auto pattern = expression.substr(start);
    if (pattern.empty() || pattern.find(u':') != pattern.npos) {
        last_error = 123;
        return UINT32_MAX;
    }
    const auto path = resolve_guest_path(start ? expression.substr(0, start) : u".");
    if (path.empty())
        return UINT32_MAX;
    DIR* directory = opendir(path.c_str());
    if (!directory) {
        last_error = errno == ENOENT ? 3 : file_error(errno);
        return UINT32_MAX;
    }
    if (pattern == u"*.*")
        pattern = u"*";
    GuestSearch search{directory, fold_ascii(pattern)};
    if (next_search == 0x40000000 || !next_file(search, output)) {
        last_error = next_search == 0x40000000 ? 4 : last_error == 18 ? 2 : last_error;
        closedir(directory);
        return UINT32_MAX;
    }
    const uint32_t handle = next_search++;
    searches.emplace(handle, std::move(search));
    return handle;
}

static uint32_t win_FindNextFileW(uint32_t *sp)
{
    const uint32_t handle = pop32(sp), output = pop32(sp);
    const auto search = searches.find(handle);
    if (search == searches.end() || !output) {
        last_error = search == searches.end() ? 6 : 87;
        return 0;
    }
    return next_file(search->second, output);
}

static uint32_t win_FindClose(uint32_t *sp)
{
    const auto search = searches.find(pop32(sp));
    if (search == searches.end()) {
        last_error = 6;
        return 0;
    }
    closedir(search->second.directory);
    searches.erase(search);
    return 1;
}

static timespec linux_file_time(uint32_t address)
{
    if (!address)
        return {0, UTIME_OMIT};
    uint64_t value;
    memcpy(&value, memory(address, sizeof value, 0), sizeof value);
    if (value == UINT64_MAX)
        return {0, UTIME_OMIT};
    return {time_t(value / 10000000) - 11644473600LL, long(value % 10000000) * 100};
}

static uint32_t win_SetFileTime(uint32_t *sp)
{
    const uint32_t handle = pop32(sp), creation = pop32(sp), access = pop32(sp), write_time = pop32(sp);
    const auto file = files.find(handle);
    if (file == files.end() || !(file->second.access & 2) || creation) {
        last_error = file == files.end() ? 6 : creation ? 50 : 5;
        return 0;
    }
    const timespec times[] = {linux_file_time(access), linux_file_time(write_time)};
    if (futimens(file->second.fd, times) == 0)
        return 1;
    last_error = file_error(errno);
    return 0;
}
)MOST";
