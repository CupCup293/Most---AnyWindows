#pragma once
inline constexpr auto runtimeDeclarations = R"MOST(
#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <vector>
#include <stdio.h>
#include <fcntl.h>
#include <dirent.h>
#include <iconv.h>
#include <locale.h>
#include <wchar.h>
#include <wctype.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <sys/statvfs.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>
#include <spawn.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <sys/mman.h>
#include <chrono>
#include <thread>
#include <mutex>

struct region {
    uint32_t address;
    size_t length;
    uint8_t *data;
    int writable;
};
)MOST";
inline constexpr auto runtimeFunctions = R"MOST(
/* Included once, after the generated guest memory regions. */
static thread_local uint32_t guest_pc;
static bool trace_guest()
{
    static const bool enabled = getenv("MOST_PE32_TRACE") != nullptr;
    return enabled;
}

static void trace_guest_call(const char* name)
{
    if (trace_guest())
        fprintf(stderr, "Most PE32: 0x%08x %s\n", guest_pc, name);
}

[[noreturn]] static void guest_fault(void)
{
    char message[128];
    const int length = snprintf(message, sizeof message,
        "Most: unsupported guest memory or CPU state at 0x%08x\n", guest_pc);
    if (length > 0 && size_t(length) < sizeof message)
        (void)write(STDERR_FILENO, message, length);
    _Exit(125);
}

struct GlobalBlock {
    std::vector<uint8_t> data;
    bool movable;
    unsigned locks = 0;
    bool clipboard_owned = false;
};
static std::map<uint32_t, GlobalBlock> global_blocks;
static uint32_t next_global_address = 0x80000000u;
static size_t global_bytes;
static int host_argc;
static char** host_argv;
static std::u16string command_line;
static constexpr uint32_t command_line_address = 0xc0000000u;

static std::map<uint32_t, std::vector<uint8_t>> guest_thread_stacks;
static std::mutex guest_execution_mutex;
static thread_local bool guest_main_thread = true;

struct GuestBlocking {
    GuestBlocking()
    {
#ifdef MOST_GUEST_THREADS
        guest_execution_mutex.unlock();
#endif
    }
    ~GuestBlocking()
    {
#ifdef MOST_GUEST_THREADS
        guest_execution_mutex.lock();
#endif
    }
};

static uint8_t *memory(uint32_t address, uint32_t count, int writing)
{
    uint64_t end = (uint64_t)address + count;
    for (size_t i = 0; import_slots[i]; ++i) {
        if (address < (uint64_t)import_slots[i] + 4 && end > import_slots[i])
            guest_fault();
    }
    for (size_t i = 0; i < sizeof regions / sizeof regions[0]; ++i) {
        const struct region *region = &regions[i];
        if (address >= region->address && end <= (uint64_t)region->address + region->length
            && (!writing || region->writable))
            return region->data + (address - region->address);
    }
    if (!writing && !command_line.empty() && address >= command_line_address &&
        end <= uint64_t(command_line_address) + command_line.size() * 2)
        return reinterpret_cast<uint8_t*>(command_line.data()) + (address - command_line_address);
    auto thread_stack = guest_thread_stacks.upper_bound(address);
    if (thread_stack != guest_thread_stacks.begin()) {
        --thread_stack;
        if (end <= uint64_t(thread_stack->first) + thread_stack->second.size())
            return thread_stack->second.data() + (address - thread_stack->first);
    }
    auto block = global_blocks.upper_bound(address);
    if (block != global_blocks.begin()) {
        --block;
        auto& entry = block->second;
        if (end <= uint64_t(block->first) + entry.data.size() && (!entry.movable || entry.locks)
            && (!writing || !entry.clipboard_owned))
            return entry.data.data() + (address - block->first);
    }
    guest_fault();
}

static uint32_t load32(uint32_t address)
{
#ifdef MOST_DYNAMIC_IMPORTS
    if (const auto value = read_import_address(address))
        return value;
#endif
    uint32_t value;
    memcpy(&value, memory(address, sizeof value, 0), sizeof value);
    return value;
}

static void store32(uint32_t address, uint32_t value)
{
    memcpy(memory(address, sizeof value, 1), &value, sizeof value);
}

static uint8_t load8(uint32_t address) { return *memory(address, 1, 0); }
static void store8(uint32_t address, uint8_t value) { *memory(address, 1, 1) = value; }
static void store16(uint32_t address, uint16_t value)
{
    memcpy(memory(address, sizeof value, 1), &value, sizeof value);
}

static void push32(uint32_t *sp, uint32_t value)
{
    *sp -= 4;
    store32(*sp, value);
}

static uint32_t pop32(uint32_t *sp)
{
    uint32_t value = load32(*sp);
    *sp += 4;
    return value;
}

static thread_local uint32_t last_error;

static uint32_t win_GlobalAlloc(uint32_t *sp)
{
    const uint32_t flags = pop32(sp), count = pop32(sp);
    if (flags & ~0x42u) {
        last_error = 87;
        return 0;
    }
    const bool movable = flags & 2;
    const uint32_t size = movable ? count : std::max(count, 1u);
    constexpr size_t limit = 64 * 1024 * 1024;
    const uint64_t extent = (uint64_t(std::max(size, 1u)) + 7) & ~uint64_t(7);
    if (size > limit - global_bytes || extent > 0xc0000000u - next_global_address ||
        global_blocks.size() >= 65536) {
        last_error = 8;
        return 0;
    }
    const uint32_t address = next_global_address;
    try {
        global_blocks.emplace(address, GlobalBlock{std::vector<uint8_t>(size), movable});
    } catch (const std::bad_alloc&) {
        last_error = 8;
        return 0;
    }
    next_global_address += extent;
    global_bytes += size;
    return address | uint32_t(movable);
}

static auto global_block(uint32_t handle)
{
    const auto block = global_blocks.find(handle & ~1u);
    return block != global_blocks.end() && block->second.movable == bool(handle & 1) ?
        block : global_blocks.end();
}

static uint32_t win_GlobalLock(uint32_t *sp)
{
    const auto block = global_block(pop32(sp));
    if (block == global_blocks.end() || block->second.data.empty()) {
        last_error = block == global_blocks.end() ? 6 : 157;
        return 0;
    }
    if (block->second.movable && block->second.locks < 255)
        ++block->second.locks;
    return block->first;
}

static uint32_t win_GlobalUnlock(uint32_t *sp)
{
    const auto block = global_block(pop32(sp));
    if (block == global_blocks.end()) {
        last_error = 6;
        return 0;
    }
    if (!block->second.movable)
        return 1;
    if (!block->second.locks) {
        last_error = 158;
        return 0;
    }
    if (--block->second.locks)
        return 1;
    last_error = 0;
    return 0;
}

static bool is_task_allocation(uint32_t);

static uint32_t win_GlobalFree(uint32_t *sp)
{
    const uint32_t handle = pop32(sp);
    if (!handle)
        return 0;
    const auto block = global_block(handle);
    if (block == global_blocks.end()) {
        last_error = 6;
        return handle;
    }
    if (block->second.clipboard_owned || is_task_allocation(handle)) {
        last_error = 5;
        return handle;
    }
    global_bytes -= block->second.data.size();
    global_blocks.erase(block);
    return 0;
}

static uint32_t win_GetLastError(uint32_t *sp)
{
    (void)sp;
    return last_error;
}

static uint32_t win_GetCurrentProcess(uint32_t *)
{
    return UINT32_MAX;
}

static uint32_t win_Sleep(uint32_t *sp)
{
    const uint32_t milliseconds = pop32(sp);
    const GuestBlocking blocking;
    if (milliseconds == 0) {
        sched_yield();
    } else if (milliseconds == UINT32_MAX) {
        for (;;)
            pause();
    } else {
        timespec delay{time_t(milliseconds / 1000), long(milliseconds % 1000) * 1000000};
        while (nanosleep(&delay, &delay) < 0 && errno == EINTR) {}
    }
    return 0;
}

static uint32_t win_GetTickCount(uint32_t *)
{
    timespec now{};
    if (clock_gettime(CLOCK_BOOTTIME, &now))
        guest_fault();
    return uint32_t(uint64_t(now.tv_sec) * 1000 + now.tv_nsec / 1000000);
}

static uint32_t win_CompareFileTime(uint32_t *sp)
{
    const uint32_t first = pop32(sp);
    const uint32_t second = pop32(sp);
    uint64_t a, b;
    memcpy(&a, memory(first, sizeof a, 0), sizeof a);
    memcpy(&b, memory(second, sizeof b, 0), sizeof b);
    return uint32_t((a > b) - (a < b));
}

static uint32_t win_MulDiv(uint32_t *sp)
{
    const int32_t a = int32_t(pop32(sp));
    const int32_t b = int32_t(pop32(sp));
    int64_t divisor = int32_t(pop32(sp));
    if (!divisor)
        return UINT32_MAX;
    int64_t product = int64_t(a) * b;
    if (divisor < 0) {
        divisor = -divisor;
        product = -product;
    }
    const int64_t rounded = (product + (product < 0 ? -divisor / 2 : divisor / 2)) / divisor;
    return rounded < INT32_MIN || rounded > INT32_MAX ? UINT32_MAX : uint32_t(rounded);
}

static uint16_t load16(uint32_t address)
{
    uint16_t value;
    memcpy(&value, memory(address, sizeof value, 0), sizeof value);
    return value;
}

static bool read_guest_wide(uint32_t address, std::u16string& text)
{
    if (!address) {
        last_error = 87;
        return false;
    }
    for (unsigned i = 0; i < 32767; ++i) {
        const uint16_t unit = load16(address + i * 2);
        if (!unit)
            return true;
        text += char16_t(unit);
    }
    last_error = 206;
    return false;
}

static std::filesystem::path resolve_guest_path(std::u16string name)
{
    std::replace(name.begin(), name.end(), u'\\', u'/');
    if (name.empty() || name.starts_with(u"//")) {
        last_error = 123;
        return {};
    }
    if (name.size() >= 2 && name[1] == u':') {
        if (name[0] != u'Z' && name[0] != u'z') {
            last_error = 15;
            return {};
        }
        name.erase(0, 2);
    }
    if (name.find_first_of(u":*?<>|\"") != std::u16string::npos) {
        last_error = 123;
        return {};
    }
    try {
        auto path = std::filesystem::path(name);
        if (path.is_relative()) {
            std::error_code error;
            auto current = std::filesystem::current_path(error);
            if (error) {
                last_error = 3;
                return {};
            }
            path = name.empty() ? current : current / path;
        }
        return path.lexically_normal();
    } catch (const std::filesystem::filesystem_error&) {
        last_error = 123;
        return {};
    }
}

static std::filesystem::path guest_path(uint32_t address)
{
    std::u16string name;
    return read_guest_wide(address, name) ? resolve_guest_path(name) : std::filesystem::path{};
}

static uint32_t win_SetCurrentDirectoryW(uint32_t *sp)
{
    const auto path = guest_path(pop32(sp));
    if (path.empty())
        return 0;
    if (chdir(path.c_str()) == 0)
        return 1;
    last_error = errno == EACCES ? 5 : 3;
    return 0;
}

static uint32_t win_GetFileAttributesW(uint32_t *sp)
{
    const auto path = guest_path(pop32(sp));
    if (path.empty())
        return UINT32_MAX;
    struct stat info;
    if (stat(path.c_str(), &info) != 0) {
        const int error = errno;
        if (error == ENOENT) {
            struct stat parent;
            last_error = stat(path.parent_path().c_str(), &parent) == 0 && S_ISDIR(parent.st_mode) ? 2 : 3;
        } else {
            last_error = error == ENOTDIR ? 3 : error == ENAMETOOLONG ? 206 : 5;
        }
        return UINT32_MAX;
    }
    if (S_ISDIR(info.st_mode))
        return 0x10;
    if (S_ISREG(info.st_mode))
        return info.st_mode & 0222 ? 0x80 : 1;
    last_error = 50;
    return UINT32_MAX;
}

static std::u16string windows_path(const std::filesystem::path& path)
{
    auto wide = u"Z:" + path.u16string();
    std::replace(wide.begin(), wide.end(), u'/', u'\\');
    return wide;
}

static void store_wide(uint32_t output, const char16_t* value, uint32_t length)
{
    memcpy(memory(output, (length + 1) * sizeof(char16_t), 1), value,
           length * sizeof(char16_t));
    const char16_t terminator = 0;
    memcpy(memory(output + length * sizeof(char16_t), sizeof terminator, 1),
           &terminator, sizeof terminator);
}

static uint32_t win_GetFullPathNameW(uint32_t *sp)
{
    const auto path = guest_path(pop32(sp));
    const uint32_t capacity = pop32(sp);
    const uint32_t output = pop32(sp);
    const uint32_t part = pop32(sp);
    if (path.empty())
        return 0;
    try {
        const auto wide = windows_path(path);
        const uint32_t length = wide.size();
        if (capacity <= length)
            return length + 1;
        store_wide(output, wide.data(), length);
        if (part)
            store32(part, wide.back() == u'\\' ? 0 :
                output + (wide.find_last_of(u'\\') + 1) * sizeof(char16_t));
        return length;
    } catch (const std::filesystem::filesystem_error&) {
        last_error = 123;
        return 0;
    }
}

static uint32_t win_GetModuleFileNameW(uint32_t *sp)
{
    const uint32_t module = pop32(sp), output = pop32(sp), capacity = pop32(sp);
    const bool invalid_module = module && module != guest_image_base;
    if (invalid_module || !output || !capacity) {
        last_error = invalid_module ? 6 : 87;
        return 0;
    }
    std::error_code error;
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
    if (error) {
        last_error = 2;
        return 0;
    }
    try {
        const auto wide = windows_path(executable);
        const uint32_t length = std::min<size_t>(wide.size(), capacity - 1);
        store_wide(output, wide.data(), length);
        if (length != wide.size()) {
            last_error = 122;
            return capacity;
        }
        return length;
    } catch (const std::filesystem::filesystem_error&) {
        last_error = 123;
        return 0;
    }
}

static uint32_t win_GetVersion(uint32_t *)
{
    // Default GetVersion view for an application without a Windows 10 manifest.
    return (9200u << 16) | 0x0206u;
}

static uint32_t win_SetErrorMode(uint32_t *sp)
{
    static uint32_t mode;
    const uint32_t previous = mode;
    mode = pop32(sp) & 0x8007u;
    return previous;
}

static std::map<std::u16string, std::u16string> guest_environment;

static uint32_t win_SetEnvironmentVariableW(uint32_t *sp)
{
    const uint32_t name_address = pop32(sp), value_address = pop32(sp);
    std::u16string name, value;
    if (!read_guest_wide(name_address, name))
        return 0;
    if (name.empty() || name.find(u'=') != std::u16string::npos) {
        last_error = 87;
        return 0;
    }
    for (auto& unit : name)
        if (unit >= u'a' && unit <= u'z')
            unit -= u'a' - u'A';
    if (!value_address) {
        if (guest_environment.erase(name))
            return 1;
        last_error = 203;
        return 0;
    }
    if (!read_guest_wide(value_address, value))
        return 0;
    guest_environment[name] = value;
    return 1;
}

static uint32_t win_GetTempPathW(uint32_t *sp)
{
    const uint32_t capacity = pop32(sp), output = pop32(sp);
    try {
        const char* temp = getenv("TMPDIR");
        const std::filesystem::path local = temp && temp[0] == '/' ? temp : "/tmp";
        auto wide = windows_path(local);
        for (const auto* key : {u"TMP", u"TEMP", u"USERPROFILE"}) {
            const auto entry = guest_environment.find(key);
            if (entry != guest_environment.end() && !entry->second.empty()) {
                const auto path = resolve_guest_path(entry->second);
                if (path.empty())
                    return 0;
                wide = windows_path(path);
                break;
            }
        }
        if (wide.back() != u'\\')
            wide += u'\\';
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

static uint32_t win_GetDiskFreeSpaceW(uint32_t *sp)
{
    const uint32_t root = pop32(sp), sectors = pop32(sp), bytes = pop32(sp);
    const uint32_t available = pop32(sp), total = pop32(sp);
    if (!sectors || !bytes || !available || !total) {
        last_error = 87;
        return 0;
    }
    const auto path = root ? guest_path(root) : std::filesystem::path(".");
    if (path.empty())
        return 0;
    struct statvfs info;
    if (statvfs(path.c_str(), &info) != 0) {
        last_error = errno == EACCES ? 5 : 3;
        return 0;
    }
    constexpr uint32_t sector_size = 512;
    uint64_t cluster_sectors = info.f_frsize / sector_size;
    uint64_t blocks = info.f_blocks, free_blocks = std::min(info.f_bavail, info.f_blocks);
    while (blocks > UINT32_MAX) {
        blocks /= 2;
        free_blocks /= 2;
        cluster_sectors *= 2;
    }
    if (!cluster_sectors || info.f_frsize % sector_size || cluster_sectors > UINT32_MAX) {
        last_error = 50;
        return 0;
    }
    store32(sectors, uint32_t(cluster_sectors));
    store32(bytes, sector_size);
    store32(available, uint32_t(free_blocks));
    store32(total, uint32_t(blocks));
    return 1;
}

template <typename Unit>
static uint32_t copy_guest_string(uint32_t output, uint32_t input, uint32_t limit)
{
    if (!output || !input)
        return 0;
    uint32_t length = 0;
    Unit unit = 0;
    while (length < limit) {
        memcpy(&unit, memory(input + length * sizeof(Unit), sizeof unit, 0), sizeof unit);
        if (!unit)
            break;
        memcpy(memory(output + length * sizeof(Unit), sizeof unit, 1), &unit, sizeof unit);
        ++length;
    }
    unit = 0;
    memcpy(memory(output + length * sizeof(Unit), sizeof unit, 1), &unit, sizeof unit);
    return output;
}

static uint32_t win_lstrcpyA(uint32_t *sp)
{
    const uint32_t output = pop32(sp), input = pop32(sp);
    return copy_guest_string<char>(output, input, UINT32_MAX);
}

static uint32_t win_lstrcpyW(uint32_t *sp)
{
    const uint32_t output = pop32(sp), input = pop32(sp);
    return copy_guest_string<char16_t>(output, input, UINT32_MAX);
}

static uint32_t win_lstrcatW(uint32_t *sp)
{
    const uint32_t output = pop32(sp), input = pop32(sp);
    if (!output || !input)
        return 0;
    uint32_t end = output;
    while (load16(end))
        end += sizeof(char16_t);
    return copy_guest_string<char16_t>(end, input, UINT32_MAX) ? output : 0;
}

static uint32_t win_lstrcpynW(uint32_t *sp)
{
    const uint32_t output = pop32(sp), input = pop32(sp);
    const int32_t capacity = int32_t(pop32(sp));
    return capacity > 0 ? copy_guest_string<char16_t>(output, input, capacity - 1) : output;
}

static uint32_t win_lstrlenA(uint32_t *sp)
{
    const uint32_t address = pop32(sp);
    uint32_t length = 0;
    if (address)
        while (*memory(address + length, 1, 0))
            ++length;
    return length;
}

static uint32_t win_lstrlenW(uint32_t *sp)
{
    const uint32_t address = pop32(sp);
    uint32_t length = 0;
    if (address)
        while (load16(address + length * 2))
            ++length;
    return length;
}

static uint32_t win_CharNextW(uint32_t *sp)
{
    const uint32_t address = pop32(sp);
    return load16(address) ? address + 2 : address;
}

static uint32_t win_CharPrevW(uint32_t *sp)
{
    const uint32_t start = pop32(sp);
    const uint32_t current = pop32(sp);
    return current > start ? current - 2 : start;
}

static uint32_t win_GetStdHandle(uint32_t *sp)
{
    uint32_t id = pop32(sp);
    if (id >= (uint32_t)-12 && id <= (uint32_t)-10)
        return (uint32_t)-id;
    last_error = 87; /* ERROR_INVALID_PARAMETER */
    return UINT32_MAX;
}

struct GuestFile {
    int fd;
    uint32_t access, share;
    dev_t device;
    ino_t inode;
};

static std::map<uint32_t, GuestFile> files;
static std::map<std::pair<dev_t, ino_t>, std::filesystem::path> delete_pending;
static std::map<uint32_t, uint32_t> guest_tokens;
static uint32_t next_file_handle = 0x10000;

static uint32_t file_error(int error)
{
    switch (error) {
    case ENOENT: return 2;
    case ENOTDIR: return 3;
    case EACCES: case EPERM: case EROFS: case ELOOP: case EISDIR: return 5;
    case EBADF: return 6;
    case EMFILE: case ENFILE: return 4;
    case EEXIST: return 80;
    case EXDEV: return 17;
    case ENOTEMPTY: return 145;
    case ENOSPC: case EDQUOT: return 112;
    case ENAMETOOLONG: return 206;
    default: return 31;
    }
}

static bool share_conflict(const struct stat& info, uint32_t access, uint32_t share)
{
    return std::any_of(files.begin(), files.end(), [&](const auto& entry) {
        const auto& file = entry.second;
        return file.device == info.st_dev && file.inode == info.st_ino &&
            ((access & ~file.share) || (file.access & ~share));
    });
}

static uint32_t move_path(uint32_t source, uint32_t destination, uint32_t flags)
{
    if (flags & ~1u) {
        last_error = 50;
        return 0;
    }
    const auto from = guest_path(source);
    if (from.empty())
        return 0;
    const auto to = guest_path(destination);
    if (to.empty())
        return 0;

    struct stat info;
    if (lstat(from.c_str(), &info) != 0) {
        last_error = file_error(errno);
        return 0;
    }
    if (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode)) {
        last_error = 5;
        return 0;
    }
    if (share_conflict(info, 4, 7)) {
        last_error = 32;
        return 0;
    }

    const bool replace = (flags & 1) && S_ISREG(info.st_mode);
    if (replace) {
        struct stat target;
        if (lstat(to.c_str(), &target) == 0) {
            if (!S_ISREG(target.st_mode) || !(target.st_mode & 0222)) {
                last_error = 5;
                return 0;
            }
            if (share_conflict(target, 4, 7)) {
                last_error = 32;
                return 0;
            }
        } else if (errno != ENOENT) {
            last_error = file_error(errno);
            return 0;
        }
    }
    if (renameat2(AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), replace ? 0 : RENAME_NOREPLACE) == 0)
        return 1;
    last_error = errno == ENOSYS || errno == EOPNOTSUPP ? 50 : file_error(errno);
    return 0;
}

static uint32_t win_MoveFileW(uint32_t *sp)
{
    const uint32_t source = pop32(sp), destination = pop32(sp);
    return move_path(source, destination, 0);
}

static uint32_t win_MoveFileExW(uint32_t *sp)
{
    const uint32_t source = pop32(sp), destination = pop32(sp), flags = pop32(sp);
    return move_path(source, destination, flags);
}

static uint32_t win_DeleteFileW(uint32_t *sp)
{
    const auto path = guest_path(pop32(sp));
    if (path.empty())
        return 0;
    struct stat info;
    if (lstat(path.c_str(), &info) != 0) {
        last_error = file_error(errno);
        return 0;
    }
    if ((!S_ISREG(info.st_mode) && !S_ISLNK(info.st_mode)) || !(info.st_mode & 0222)) {
        last_error = 5;
        return 0;
    }
    if (share_conflict(info, 4, 7)) {
        last_error = 32;
        return 0;
    }
    if (unlink(path.c_str()) == 0)
        return 1;
    last_error = file_error(errno);
    return 0;
}

static uint32_t win_CreateDirectoryW(uint32_t *sp)
{
    const uint32_t name = pop32(sp), security = pop32(sp);
    if (security) {
        last_error = 50;
        return 0;
    }
    const auto path = guest_path(name);
    if (path.empty())
        return 0;
    if (mkdir(path.c_str(), 0777) == 0)
        return 1;
    last_error = errno == EEXIST ? 183 : file_error(errno);
    return 0;
}

static uint32_t win_RemoveDirectoryW(uint32_t *sp)
{
    const auto path = guest_path(pop32(sp));
    if (path.empty())
        return 0;
    std::error_code error;
    if (std::filesystem::equivalent(path, ".", error)) {
        last_error = 16;
        return 0;
    }
    if (rmdir(path.c_str()) == 0)
        return 1;
    last_error = errno == ENOENT ? 3 : file_error(errno);
    return 0;
}

static int reference_regular_file(const std::filesystem::path& path, struct stat& info)
{
    const int reference = open(path.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC);
    if (reference < 0)
        return -1;
    const int status = fstat(reference, &info);
    if (!status && S_ISREG(info.st_mode))
        return reference;
    const int error = status ? errno : EACCES;
    close(reference);
    errno = error;
    return -1;
}

static std::string fd_path(int fd)
{
    return "/proc/self/fd/" + std::to_string(fd);
}

static int open_existing_file(const std::filesystem::path& path, int mode)
{
    // Inspect a pinned object before opening it for I/O.
    struct stat info;
    const int reference = reference_regular_file(path, info);
    if (reference < 0)
        return -1;
    const int fd = open(fd_path(reference).c_str(), mode & ~O_NOFOLLOW);
    const int error = errno;
    close(reference);
    errno = error;
    return fd;
}

static uint32_t win_SetFileAttributesW(uint32_t *sp)
{
    const uint32_t name = pop32(sp), attributes = pop32(sp);
    if (attributes != 1 && attributes != 0x80) {
        last_error = 50;
        return 0;
    }
    const auto path = guest_path(name);
    if (path.empty())
        return 0;
    struct stat info;
    const int reference = reference_regular_file(path, info);
    if (reference < 0) {
        last_error = file_error(errno);
        return 0;
    }
    const mode_t original = info.st_mode & 0777;
    const mode_t mode = attributes == 1 ? original & ~0222u :
        (original & 0222u) ? original : original | S_IWUSR;
    const int result = original == mode ? 0 : chmod(fd_path(reference).c_str(), mode);
    const int error = errno;
    close(reference);
    if (result != 0)
        last_error = file_error(error);
    return result == 0;
}

static int copy_regular_file(int input, const struct stat& source, const std::filesystem::path& destination,
                             bool fail_if_exists)
{
    struct stat target;
    const bool exists = lstat(destination.c_str(), &target) == 0;
    if (!exists && errno != ENOENT)
        return file_error(errno);
    if (exists) {
        if (fail_if_exists)
            return 80;
        if (!S_ISREG(target.st_mode) || !(target.st_mode & 0222) || target.st_nlink != 1 ||
            (target.st_dev == source.st_dev && target.st_ino == source.st_ino))
            return 5;
        if (share_conflict(target, 6, 7))
            return 32;
        if (faccessat(AT_FDCWD, destination.c_str(), W_OK, AT_EACCESS) != 0)
            return file_error(errno);
    }

    auto temporary = (destination.parent_path() / ".most-copy-XXXXXX").native();
    const int output = mkostemp(temporary.data(), O_CLOEXEC);
    if (output < 0)
        return file_error(errno);
    int error = 0;
    off_t remaining = source.st_size;
    while (remaining > 0) {
        const ssize_t copied = sendfile(output, input, nullptr, std::min<off_t>(remaining, 1 << 20));
        if (copied < 0 && errno == EINTR)
            continue;
        if (copied <= 0) {
            error = copied == 0 ? 30 : file_error(errno);
            break;
        }
        remaining -= copied;
    }
    const timespec times[] = {source.st_atim, source.st_mtim};
    if (!error && (fchmod(output, source.st_mode & 0222 ? 0600 : 0400) != 0 ||
                   futimens(output, times) != 0))
        error = file_error(errno);
    if (close(output) != 0 && !error)
        error = file_error(errno);

    if (!error && exists) {
        struct stat current;
        if (lstat(destination.c_str(), &current) != 0 ||
            current.st_dev != target.st_dev || current.st_ino != target.st_ino)
            error = 32;
    }
    if (!error && renameat2(AT_FDCWD, temporary.c_str(), AT_FDCWD, destination.c_str(),
                            exists ? 0 : RENAME_NOREPLACE) == 0)
        return 0;
    if (!error)
        error = errno == ENOSYS || errno == EOPNOTSUPP ? 50 : file_error(errno);
    unlink(temporary.c_str());
    return error;
}

static uint32_t win_CopyFileW(uint32_t *sp)
{
    const uint32_t source = pop32(sp), destination = pop32(sp);
    const bool fail_if_exists = pop32(sp) != 0;
    const auto from = guest_path(source);
    if (from.empty())
        return 0;
    const auto to = guest_path(destination);
    if (to.empty())
        return 0;
    const int input = open_existing_file(from, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (input < 0) {
        last_error = file_error(errno);
        return 0;
    }
    struct stat info;
    const int error = fstat(input, &info) != 0 ? file_error(errno) :
        share_conflict(info, 1, 7) ? 32 : copy_regular_file(input, info, to, fail_if_exists);
    close(input);
    if (error)
        last_error = error;
    return error == 0;
}

#ifdef MOST_EMBEDDED_PE
static bool is_guest_executable(const std::filesystem::path& path)
{
    std::error_code error;
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
    return !error && path == executable;
}

static int guest_executable_fd()
{
    static int image = -1;
    if (image < 0) {
        const int fd = memfd_create("most-original-pe", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        if (fd < 0)
            return -1;
        size_t position = 0;
        while (position < guest_original_pe_size) {
            const auto count = write(fd, guest_original_pe + position, guest_original_pe_size - position);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                break;
            position += count;
        }
        if (position != guest_original_pe_size
            || fcntl(fd, F_ADD_SEALS, F_SEAL_SEAL | F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK) < 0) {
            const int error = errno ? errno : EIO;
            close(fd);
            errno = error;
            return -1;
        }
        image = fd;
    }
    const auto path = "/proc/self/fd/" + std::to_string(image);
    return open(path.c_str(), O_RDONLY | O_CLOEXEC);
}
#endif

static uint32_t win_CreateFileW(uint32_t *sp)
{
    const uint32_t name = pop32(sp);
    const uint32_t access = pop32(sp), share = pop32(sp), security = pop32(sp);
    const uint32_t disposition = pop32(sp), flags = pop32(sp), template_file = pop32(sp);
    constexpr uint32_t read_access = 0x80000000u, write_access = 0x40000000u;
    const bool delete_on_close = flags & 0x04000000u;
    if (disposition < 1 || disposition > 5 || (access & ~(read_access | write_access)) ||
        (share & ~7u) || security || template_file || (flags & ~(0x80u | 0x20u | 0x100u | 0x04000000u | 0x08000000u))) {
        last_error = 87;
        return UINT32_MAX;
    }
    const bool truncate = disposition == 2 || disposition == 5;
    if (truncate && !(access & write_access)) {
        last_error = 5;
        return UINT32_MAX;
    }
    const auto path = guest_path(name);
    if (path.empty())
        return UINT32_MAX;
    if (next_file_handle == UINT32_MAX) {
        last_error = 4;
        return UINT32_MAX;
    }
    const int mode = O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK |
        ((access & write_access) ? ((access & read_access) ? O_RDWR : O_WRONLY) : O_RDONLY);
    const bool create = disposition == 1 || disposition == 2 || disposition == 4;
    int fd;
    bool existing;
#ifdef MOST_EMBEDDED_PE
    if (is_guest_executable(path)) {
        if ((access & write_access) || delete_on_close || (disposition != 3 && disposition != 4)) {
            last_error = disposition == 1 ? 80 : 5;
            return UINT32_MAX;
        }
        fd = guest_executable_fd();
        existing = true;
    } else
#endif
    {
        fd = create ? open(path.c_str(), mode | O_CREAT | O_EXCL, 0666) : open_existing_file(path, mode);
        existing = !create;
    }
    if (fd < 0 && errno == EEXIST && (disposition == 2 || disposition == 4)) {
        existing = true;
        fd = open_existing_file(path, mode);
    }
    if (fd < 0) {
        last_error = file_error(errno);
        return UINT32_MAX;
    }
    struct stat info;
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
        close(fd);
        last_error = 5;
        return UINT32_MAX;
    }
    const uint32_t requested = ((access & read_access) ? 1u : 0u)
        | ((access & write_access) ? 2u : 0u) | (delete_on_close ? 4u : 0u);
    const auto key = std::make_pair(info.st_dev, info.st_ino);
    if (delete_pending.contains(key) && !(share & 4)) {
        close(fd);
        last_error = 5;
        return UINT32_MAX;
    }
    if (share_conflict(info, requested, share)) {
        close(fd);
        last_error = 32;
        return UINT32_MAX;
    }
    if (existing && truncate && ftruncate(fd, 0) != 0) {
        last_error = file_error(errno);
        close(fd);
        return UINT32_MAX;
    }
    const uint32_t handle = next_file_handle++;
    files.emplace(handle, GuestFile{fd, requested, share, info.st_dev, info.st_ino});
    if (delete_on_close)
        delete_pending.try_emplace(key, path);
    if (disposition == 2 || disposition == 4)
        last_error = existing ? 183 : 0;
    return handle;
}

static bool close_process_handle(uint32_t handle);
static bool close_thread_handle(uint32_t handle);

static bool close_guest_file(std::map<uint32_t, GuestFile>::iterator file)
{
    const auto key = std::make_pair(file->second.device, file->second.inode);
    const int result = close(file->second.fd);
    int error = result ? errno : 0;
    files.erase(file);
    const auto pending = delete_pending.find(key);
    if (pending != delete_pending.end() && std::none_of(files.begin(), files.end(), [&](const auto& item) {
            return item.second.device == key.first && item.second.inode == key.second;
        })) {
        struct stat current{};
        if (lstat(pending->second.c_str(), &current) == 0
            && current.st_dev == key.first && current.st_ino == key.second
            && unlink(pending->second.c_str()) != 0 && !error)
            error = errno;
        delete_pending.erase(pending);
    }
    if (error)
        last_error = file_error(error);
    return !error;
}

static void finish_files()
{
    while (!files.empty())
        close_guest_file(files.begin());
}

static uint32_t win_CloseHandle(uint32_t *sp)
{
    const auto handle = pop32(sp);
    if (guest_tokens.erase(handle) || close_process_handle(handle) || close_thread_handle(handle))
        return 1;
    const auto file = files.find(handle);
    if (file == files.end()) {
        last_error = 6;
        return 0;
    }
    return close_guest_file(file);
}

static uint32_t win_GetFileSize(uint32_t *sp)
{
    const auto file = files.find(pop32(sp));
    const uint32_t high = pop32(sp);
    if (file == files.end()) {
        last_error = 6;
        return UINT32_MAX;
    }
    struct stat info;
    if (fstat(file->second.fd, &info) != 0) {
        last_error = file_error(errno);
        return UINT32_MAX;
    }
    const uint64_t size = info.st_size;
    if (high)
        store32(high, uint32_t(size >> 32));
    if (uint32_t(size) == UINT32_MAX)
        last_error = 0;
    return uint32_t(size);
}

static uint32_t win_SetFilePointer(uint32_t *sp)
{
    const auto file = files.find(pop32(sp));
    const uint32_t low = pop32(sp), high = pop32(sp), origin = pop32(sp);
    if (file == files.end() || origin > 2) {
        last_error = file == files.end() ? 6 : 87;
        return UINT32_MAX;
    }
    const int fd = file->second.fd;
    int64_t base = 0;
    if (origin == 1) {
        base = lseek(fd, 0, SEEK_CUR);
        if (base < 0) {
            last_error = file_error(errno);
            return UINT32_MAX;
        }
    } else if (origin == 2) {
        struct stat info;
        if (fstat(fd, &info) != 0) {
            last_error = file_error(errno);
            return UINT32_MAX;
        }
        base = info.st_size;
    }
    const int64_t distance = high ? int64_t((uint64_t(load32(high)) << 32) | low) : int32_t(low);
    int64_t position;
    if (__builtin_add_overflow(base, distance, &position) || (!high && position > INT32_MAX)) {
        last_error = 87;
        return UINT32_MAX;
    }
    if (position < 0) {
        last_error = 131;
        return UINT32_MAX;
    }
    if (lseek(fd, position, SEEK_SET) < 0) {
        last_error = file_error(errno);
        return UINT32_MAX;
    }
    if (high)
        store32(high, uint32_t(uint64_t(position) >> 32));
    if (uint32_t(position) == UINT32_MAX)
        last_error = 0;
    return uint32_t(position);
}

static uint32_t transfer_file(uint32_t *sp, bool writing)
{
    const uint32_t handle = pop32(sp), buffer = pop32(sp), length = pop32(sp);
    const uint32_t completed = pop32(sp), overlapped = pop32(sp);
    if (!completed || overlapped) {
        last_error = 87;
        return 0;
    }
    store32(completed, 0);
    int fd;
    if ((writing && (handle == 11 || handle == 12)) || (!writing && handle == 10)) {
        fd = int(handle) - 10;
    } else {
        const auto file = files.find(handle);
        if (file == files.end()) {
            last_error = 6;
            return 0;
        }
        if (!(file->second.access & (writing ? 2u : 1u))) {
            last_error = 5;
            return 0;
        }
        fd = file->second.fd;
    }
    if (!length)
        return 1;
    auto* data = memory(buffer, length, !writing);
    ssize_t result;
    do {
        result = writing ? write(fd, data, length) : read(fd, data, length);
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
        last_error = errno == EPIPE ? 109 : file_error(errno);
        return 0;
    }
    store32(completed, uint32_t(result));
    return 1;
}

static uint32_t win_ReadFile(uint32_t *sp) { return transfer_file(sp, false); }
static uint32_t win_WriteFile(uint32_t *sp) { return transfer_file(sp, true); }
)MOST";
