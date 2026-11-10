#pragma once
inline constexpr auto runtimeSecurity = R"MOST(
static uint32_t next_token = 0x40000000;
static uint32_t change_notify = 3;
static bool change_notify_present = true;

static uint32_t win_OpenProcessToken(uint32_t *sp)
{
    const uint32_t process = pop32(sp), access = pop32(sp), output = pop32(sp);
    if (process != UINT32_MAX || !output || (access & ~(0x00020028u | 0x02000000u))) {
        last_error = process != UINT32_MAX ? 6 : !output ? 87 : 5;
        return 0;
    }
    if (next_token == 0x50000000) {
        last_error = 4;
        return 0;
    }
    const uint32_t handle = next_token++;
    guest_tokens.emplace(handle, access & 0x02000000u ? 0x28u : access);
    store32(output, handle);
    return 1;
}

static uint32_t win_LookupPrivilegeValueW(uint32_t *sp)
{
    const uint32_t system = pop32(sp), name_address = pop32(sp), output = pop32(sp);
    std::u16string host, name;
    if (!output || (system && !read_guest_wide(system, host)) || !read_guest_wide(name_address, name)) {
        last_error = 87;
        return 0;
    }
    if (!host.empty() && host != u".") {
        last_error = 50;
        return 0;
    }
    static const std::map<std::u16string, uint32_t> known{
        {u"SECREATETOKENPRIVILEGE", 2}, {u"SEASSIGNPRIMARYTOKENPRIVILEGE", 3},
        {u"SELOCKMEMORYPRIVILEGE", 4}, {u"SEINCREASEQUOTAPRIVILEGE", 5},
        {u"SEMACHINEACCOUNTPRIVILEGE", 6}, {u"SETCBPRIVILEGE", 7},
        {u"SESECURITYPRIVILEGE", 8}, {u"SETAKEOWNERSHIPPRIVILEGE", 9},
        {u"SELOADDRIVERPRIVILEGE", 10}, {u"SESYSTEMPROFILEPRIVILEGE", 11},
        {u"SESYSTEMTIMEPRIVILEGE", 12}, {u"SEPROFILESINGLEPROCESSPRIVILEGE", 13},
        {u"SEINCREASEBASEPRIORITYPRIVILEGE", 14}, {u"SECREATEPAGEFILEPRIVILEGE", 15},
        {u"SECREATEPERMANENTPRIVILEGE", 16}, {u"SEBACKUPPRIVILEGE", 17},
        {u"SERESTOREPRIVILEGE", 18}, {u"SESHUTDOWNPRIVILEGE", 19},
        {u"SEDEBUGPRIVILEGE", 20}, {u"SEAUDITPRIVILEGE", 21},
        {u"SESYSTEMENVIRONMENTPRIVILEGE", 22}, {u"SECHANGENOTIFYPRIVILEGE", 23},
        {u"SEREMOTESHUTDOWNPRIVILEGE", 24}, {u"SEUNDOCKPRIVILEGE", 25},
        {u"SESYNCAGENTPRIVILEGE", 26}, {u"SEENABLEDELEGATIONPRIVILEGE", 27},
        {u"SEMANAGEVOLUMEPRIVILEGE", 28}, {u"SEIMPERSONATEPRIVILEGE", 29},
        {u"SECREATEGLOBALPRIVILEGE", 30}, {u"SETRUSTEDCREDMANACCESSPRIVILEGE", 31},
        {u"SERELABELPRIVILEGE", 32}, {u"SEINCREASEWORKINGSETPRIVILEGE", 33},
        {u"SETIMEZONEPRIVILEGE", 34}, {u"SECREATESYMBOLICLINKPRIVILEGE", 35},
        {u"SEDELEGATESESSIONUSERIMPERSONATEPRIVILEGE", 36}};
    const auto found = known.find(fold_ascii(name));
    if (found == known.end()) {
        last_error = 1313;
        return 0;
    }
    store32(output, found->second);
    store32(output + 4, 0);
    return 1;
}

static uint32_t win_AdjustTokenPrivileges(uint32_t *sp)
{
    const uint32_t handle = pop32(sp), disable_all = pop32(sp), state = pop32(sp);
    const uint32_t capacity = pop32(sp), previous = pop32(sp), returned = pop32(sp);
    const auto token = guest_tokens.find(handle);
    if (token == guest_tokens.end() || !(token->second & 32) || (previous && !(token->second & 8))) {
        last_error = token == guest_tokens.end() ? 6 : 5;
        return 0;
    }
    if ((!disable_all && !state) || (previous && !returned)) {
        last_error = 87;
        return 0;
    }
    uint32_t next = change_notify;
    bool present = change_notify_present, all_assigned = true;
    if (disable_all) {
        next &= ~2u;
    } else {
        const uint32_t count = load32(state);
        if (count > 256) {
            last_error = 87;
            return 0;
        }
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t item = state + 4 + i * 12;
            const uint32_t low = load32(item), high = load32(item + 4), attributes = load32(item + 8);
            if (!present || low != 23 || high) {
                all_assigned = false;
            } else if (attributes & 4) {
                present = false;
            } else {
                next = (next & ~2u) | (attributes & 2u);
            }
        }
    }
    const bool changed = present && next != change_notify;
    if (previous) {
        const uint32_t required = changed ? 16 : 4;
        store32(returned, required);
        if (capacity < required) {
            last_error = 122;
            return 0;
        }
        store32(previous, changed ? 1 : 0);
        if (changed) {
            store32(previous + 4, 23);
            store32(previous + 8, 0);
            store32(previous + 12, change_notify);
        }
    }
    change_notify = next;
    change_notify_present = present;
    last_error = all_assigned ? 0 : 1300;
    return 1;
}

// Translate an allow-only DACL for the Unix owner, group and Everyone.
static int unix_acl_subject(uint32_t address, unsigned length, const struct stat& info)
{
    if (length < 12 || *memory(address, 1, 0) != 1)
        return -1;
    const auto count = *memory(address + 1, 1, 0);
    if (8u + 4u * count != length)
        return -1;
    const auto* authority = memory(address + 2, 6, 0);
    const uint8_t world[6]{0, 0, 0, 0, 0, 1}, unix_sid[6]{0, 0, 0, 0, 0, 22};
    if (count == 1 && !memcmp(authority, world, 6) && load32(address + 8) == 0)
        return 2;
    if (count != 2 || memcmp(authority, unix_sid, 6))
        return -1;
    const auto kind = load32(address + 8), id = load32(address + 12);
    if (kind == 1 && id == info.st_uid)
        return 0;
    if (kind == 2 && id == info.st_gid)
        return 1;
    return -1;
}

static uint32_t win_SetFileSecurityW(uint32_t* sp)
{
    const auto name = pop32(sp), flags = pop32(sp), descriptor = pop32(sp);
    if (!descriptor || !(flags & 4) || (flags & ~0xa0000004u)) {
        last_error = 50;
        return 0;
    }
    const auto control = load16(descriptor + 2);
    const auto location = load32(descriptor + 16);
    if (*memory(descriptor, 1, 0) != 1 || !(control & 4) || !location
        || ((control & 0x8000) && (location < 20 || location > 65535))) {
        last_error = 1338;
        return 0;
    }
    const auto dacl = control & 0x8000 ? descriptor + location : location;
    if ((control & 0x8000) && dacl < descriptor) {
        last_error = 1338;
        return 0;
    }
    const auto size = load16(dacl + 2), count = load16(dacl + 4);
    if (size < 8 || count > (size - 8u) / 20u || *memory(dacl, 1, 0) != 2) {
        last_error = 1336;
        return 0;
    }
    memory(dacl, size, 0);
    const auto path = guest_path(name);
    if (path.empty())
        return 0;
    const auto close_fd = [](int* fd) { if (*fd >= 0) close(*fd); };
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
    const std::unique_ptr<int, decltype(close_fd)> guard(&fd, close_fd);
    if (fd < 0) {
        last_error = file_error(errno);
        return 0;
    }
    struct stat info{};
    if (fstat(fd, &info) != 0 || (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode))) {
        last_error = 50;
        return 0;
    }
    unsigned access[3]{};
    unsigned offset = 8;
    for (unsigned i = 0; i < count; ++i) {
        const auto ace = dacl + offset;
        const auto length = load16(ace + 2);
        if (length < 20 || length > size - offset || load16(ace) != 0) {
            last_error = 50;
            return 0;
        }
        const auto subject = unix_acl_subject(ace + 8, length - 8, info);
        const auto mask = load32(ace + 4);
        if (subject < 0 || (mask & 0x0fffffffu)) {
            last_error = 50;
            return 0;
        }
        access[subject] |= (mask & 0x10000000u) ? 7 : (mask >> 29) & 7;
        offset += length;
    }
    const auto acl = fgetxattr(fd, "system.posix_acl_access", nullptr, 0);
    if (acl > 0 || (acl < 0 && errno != ENODATA && errno != ENOTSUP)) {
        last_error = 50;
        return 0;
    }
    const auto mode = ((access[0] | access[2]) << 6) | ((access[1] | access[2]) << 3) | access[2];
    if (fchmod(fd, (info.st_mode & 07000u) | mode) != 0) {
        last_error = file_error(errno);
        return 0;
    }
    return 1;
}

static std::map<uint32_t, std::filesystem::path> shell_items;

static bool is_task_allocation(uint32_t address) { return shell_items.contains(address); }

static uint32_t allocate_guest_blob(std::vector<uint8_t> bytes)
{
    const auto size = bytes.size();
    const auto result = next_global_address;
    const auto extent = (size + 15) & ~size_t(15);
    if (!size || size > 64 * 1024 * 1024 - global_bytes || extent > 0xc0000000u - result
        || global_blocks.size() >= 65536)
        return 0;
    global_blocks.emplace(result, GlobalBlock{std::move(bytes), false});
    next_global_address += extent;
    global_bytes += size;
    return result;
}

static void free_guest_blob(uint32_t address)
{
    const auto it = global_blocks.find(address);
    if (it != global_blocks.end()) {
        global_bytes -= it->second.data.size();
        global_blocks.erase(it);
    }
}

static uint32_t allocate_shell_item(const std::filesystem::path& path)
{
    if (shell_items.size() >= 4096)
        return 0;
    const auto wide = windows_path(path);
    if (wide.size() >= 260 || wide.empty())
        return 0;
    const auto length = uint16_t(2 + (wide.size() + 1) * 2);
    std::vector<uint8_t> bytes(length + 2);
    memcpy(bytes.data(), &length, 2);
    memcpy(bytes.data() + 2, wide.data(), wide.size() * 2);
    const auto address = allocate_guest_blob(std::move(bytes));
    if (address)
        shell_items.emplace(address, path);
    return address;
}

static uint32_t win_CoTaskMemFree(uint32_t* sp)
{
    const auto address = pop32(sp);
    if (shell_items.erase(address))
        free_guest_blob(address);
    return 0;
}

static unsigned ole_depth;

static uint32_t win_OleInitialize(uint32_t *sp)
{
    if (pop32(sp))
        return 0x80070057u;
    return ole_depth++ ? 1 : 0;
}

static uint32_t win_OleUninitialize(uint32_t *)
{
    if (ole_depth)
        --ole_depth;
    return 0;
}
)MOST";
