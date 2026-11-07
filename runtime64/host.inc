#include "registry.inc"
#include <linux/fs.h>
#include <sys/ioctl.h>

extern int native_launch, native_usb_fd;
extern const char *native_executable;
extern char native_workdir[];

static int is_work_path(const char *path)
{
    const char *root = native_launch ? native_workdir : "/scratch";
    size_t size = strlen(root);
    return !strncmp(path, root, size) && (!path[size] || path[size] == '/');
}

WIN int win_SetDefaultDllDirectories(DWORD flags)
{
    if (!flags || (flags & ~0x1e00u)) {
        last_error = 87;
        return 0;
    }
    return 1; // Library lookups are already restricted to the internal module table.
}

WIN int win_GetUserNameA(char *name, DWORD *capacity)
{
    struct passwd *pw = getpwuid(getuid());
    const char *user = pw ? pw->pw_name : "Linux";
    DWORD needed = strlen(user) + 1;
    if (!capacity || *capacity < needed || !name) {
        if (capacity)
            *capacity = needed;
        last_error = 122;
        return 0;
    }
    memcpy(name, user, needed);
    *capacity = needed;
    return 1;
}

static uintptr_t fd_handle(int fd) { return 0x10000 + (uintptr_t)fd; }
static int handle_fd(uintptr_t handle)
{
    if (handle >= 0x10003 && handle < 0x20000)
        return handle - 0x10000;
    if (handle >= 1 && handle <= 3)
        return handle - 1;
    return -1;
}

static const char probe_disk_path[] = "/scratch/device.img";
static const char probe_disk_link[] = "\\\\?\\USBSTOR#Disk&Ven_Most&Prod_Test_Media&Rev_1.00#PROBE&0#{53f56307-b6bf-11d0-94f2-00a0c91efb8b}";
static const char probe_disk_instance[] = "USBSTOR\\Disk&Ven_Most&Prod_Test_Media&Rev_1.00\\PROBE&0";
static const char probe_physical_drive[] = "\\\\.\\PhysicalDrive2";

static int probe_disk_stat(struct stat *info)
{
    if (native_usb_fd >= 0) {
        uint64_t size;
        if (fstat(native_usb_fd, info) || !S_ISBLK(info->st_mode) ||
            ioctl(native_usb_fd, BLKGETSIZE64, &size))
            return 0;
        info->st_size = size;
        return 1;
    }
    const char *enabled = getenv("RUFUS_PROBE_DEVICE");
    return enabled && !strcmp(enabled, "1") && lstat(probe_disk_path, info) == 0 &&
        S_ISREG(info->st_mode) && info->st_size >= 16 * 1024 * 1024;
}

static int open_probe_disk(int mode)
{
    if (native_usb_fd < 0)
        return open(probe_disk_path, mode | O_NOFOLLOW);
    char fd_path[64];
    snprintf(fd_path, sizeof(fd_path), "/proc/self/fd/%d", native_usb_fd);
    return open(fd_path, mode | O_NONBLOCK);
}

static const char probe_volume[] = "\\\\?\\Volume{7b2e56cb-24f4-4618-ab0b-377102c02d01}\\";
static _Atomic int probe_mounted;

static int probe_volume_sector(unsigned char sector[512])
{
    struct stat info;
    if (!probe_disk_stat(&info))
        return 0;
    if (native_usb_fd >= 0 && pread(native_usb_fd, sector, 512, 0) != 512)
        return 0;
    int fd = native_usb_fd >= 0 ? -1 : open_probe_disk(O_RDONLY | O_CLOEXEC);
    if (native_usb_fd < 0 && fd < 0)
        return 0;
    ssize_t count = fd < 0 ? 512 : pread(fd, sector, 512, 0);
    if (fd >= 0)
        close(fd);
    if (count != 512 || sector[510] != 0x55 || sector[511] != 0xaa ||
        read_word(sector + 11) != 512 || !sector[13] || !sector[16])
        return 0;
    DWORD sectors = read_word(sector + 19);
    if (!sectors)
        sectors = read_dword(sector + 32);
    return sectors && sectors <= (uint64_t)info.st_size / 512;
}
static int probe_volume_ready(void)
{
    unsigned char sector[512];
    return probe_volume_sector(sector);
}
static int is_probe_volume(const char *path)
{
    const size_t length = sizeof(probe_volume) - 2;
    return !strncasecmp(path, probe_volume, length) &&
        (!path[length] || (path[length] == '\\' && !path[length + 1]));
}

static int host_path(const WCHAR *name, char *output, size_t capacity)
{
    char path[512];
#if defined(MOST_RAWCOPY) || defined(MOST_CHSIZE) || defined(MOST_FILEINFO) || defined(MOST_REPTXT) || defined(MOST_ELTORITO)
    if (!name || !win_WideCharToMultiByte(65001, 0, name, -1, path, sizeof(path), NULL, NULL))
        return 0;
    const char *raw_path = !strncmp(path, "Z:/", 3) ? path + 2 : path;
    if (!*raw_path || strchr(raw_path, ':') || strchr(raw_path, '\\'))
        return 0;
    return snprintf(output, capacity, "%s", raw_path) < (int)capacity;
#endif
    if (!name || !(native_launch ? win_WideCharToMultiByte(65001, 0, name, -1, path, sizeof(path), NULL, NULL) : ascii_name(name, path, sizeof(path))))
        return 0;
    if (!strcasecmp(path, probe_disk_link) || !strcasecmp(path, probe_physical_drive) ||
        (is_probe_volume(path) && probe_volume_ready()) ||
        (probe_mounted && (!strcasecmp(path, "\\\\.\\C:") || !strcasecmp(path, "C:\\")))) {
        struct stat info;
        if (!probe_disk_stat(&info))
            return 0;
        return snprintf(output, capacity, "%s", probe_disk_path) < (int)capacity;
    }
    if (!strncmp(path, "\\\\?\\", 4))
        memmove(path, path + 4, strlen(path + 4) + 1);
    if (!strcasecmp(path, "Z:\\rufus.exe")) {
        snprintf(output, capacity, "/rufus");
        return 1;
    }
    char *relative = path;
    int absolute = !strncasecmp(path, "Z:\\scratch", 10) && (!path[10] || path[10] == '\\');
    if (absolute)
        relative += 10;
    else if (native_launch && !strncasecmp(path, "Z:\\", 3)) {
        relative += 2;
    } else if (strchr(path, ':') || *path == '\\' || *path == '/')
        return 0;
    for (char *p = relative; *p; ++p)
        if (*p == '\\')
            *p = '/';
    if (strstr(relative, ".."))
        return 0;
    int count = absolute ? snprintf(output, capacity, "%s%s", native_launch ? native_workdir : "/scratch", relative)
                         : snprintf(output, capacity, "%s", *relative ? relative : ".");
    return count >= 0 && (size_t)count < capacity;
}

WIN uintptr_t win_CreateFileW(const WCHAR *name, DWORD access, DWORD share, void *security,
                               DWORD disposition, DWORD flags, uintptr_t template)
{
    (void)share; (void)security; (void)flags; (void)template;
    char path[512];
    if (!host_path(name, path, sizeof(path))) {
#ifdef MOST_FILEINFO
        fputs("FileInfo could not map a path to a Linux file.\n", stderr);
        fileinfo_failed = 1;
#endif
        last_error = 2;
        return (uintptr_t)-1;
    }
    int mode = O_CLOEXEC | O_NOFOLLOW;
    if (access & 0x40000000u)
        mode |= access & 0x80000000u ? O_RDWR : O_WRONLY;
    switch (disposition) {
    case 1: mode |= O_CREAT | O_EXCL; break;
    case 2: mode |= O_CREAT | O_TRUNC; break;
    case 3: break;
    case 4: mode |= O_CREAT; break;
    case 5: mode |= O_TRUNC; break;
    default: last_error = 87; return (uintptr_t)-1;
    }
    if (!strcmp(path, probe_disk_path) && disposition != 3) {
        last_error = 5;
        return (uintptr_t)-1;
    }
    if (!strcmp(path, "/rufus") && (access & 0x40000000u || disposition != 3)) {
        last_error = 5;
        return (uintptr_t)-1;
    }
    int fd;
#ifdef MOST_FILEINFO
    if (disposition != 3 || (access & (0x40000000u | 0x110u))) {
        fprintf(stderr, "FileInfo opens files for metadata queries only: %s\n", path);
        fileinfo_failed = 1;
        last_error = 5;
        return (uintptr_t)-1;
    }
    // Metadata reads do not need permission to read the file's contents.
    fd = open(path, O_PATH | O_NOFOLLOW | O_CLOEXEC);
    struct stat info;
    if (fd < 0) {
        fprintf(stderr, "FileInfo could not open %s: %s\n", path, strerror(errno));
        fileinfo_failed = 1;
    } else if (fstat(fd, &info) || !(S_ISREG(info.st_mode) || S_ISDIR(info.st_mode))) {
        close(fd);
        fprintf(stderr, "FileInfo only inspects files and directories: %s\n", path);
        fileinfo_failed = 1;
        last_error = 5;
        return (uintptr_t)-1;
    }
#elif defined(MOST_RAWCOPY) || defined(MOST_CHSIZE) || defined(MOST_REPTXT) || defined(MOST_ELTORITO)
#ifdef MOST_ELTORITO
    if (access != 0x80000000u || disposition != 3) {
        last_error = 5;
        return (uintptr_t)-1;
    }
#endif
    struct stat opened, source;
#ifdef MOST_REPTXT
    if (disposition != 4) {
        last_error = 87;
        return (uintptr_t)-1;
    }
    mode &= ~O_CREAT; // A match deleted after enumeration must not be recreated.
#endif
    fd = open(path, (mode & ~O_TRUNC) | O_NONBLOCK, 0600);
    if (fd >= 0 && (fstat(fd, &opened) || !S_ISREG(opened.st_mode))) {
        close(fd);
        fprintf(stderr, "Only regular files are supported: %s\n", path);
        last_error = 5;
        return (uintptr_t)-1;
    }
#ifdef MOST_RAWCOPY
    int source_fd = handle_fd(template);
    if (fd >= 0 && source_fd >= 0 && fstat(source_fd, &source) == 0) {
        if (source.st_dev == opened.st_dev && source.st_ino == opened.st_ino) {
            close(fd);
            fputs("RawCopy input and output refer to the same file.\n", stderr);
            last_error = 32;
            return (uintptr_t)-1;
        }
        // RawCopy's differential loop can append a block after a short
        // comparison read. Restrict existing targets to equal-size files.
        if ((mode & O_ACCMODE) == O_RDWR && opened.st_size &&
            (!S_ISREG(source.st_mode) || opened.st_size != source.st_size)) {
            close(fd);
            fputs("RawCopy differential output must be empty or match the input size.\n", stderr);
            last_error = 5;
            return (uintptr_t)-1;
        }
    }
#endif
    if (fd >= 0 && (mode & O_TRUNC) && ftruncate(fd, 0)) {
        close(fd);
        last_error = 5;
        return (uintptr_t)-1;
    }
    if (fd < 0) {
        fprintf(stderr, "Could not open %s: %s\n", path, strerror(errno));
#ifdef MOST_REPTXT
        reptxt_failed = 1;
#endif
    }
#else
    if (!strcmp(path, probe_disk_path)) {
        fd = open_probe_disk(mode & ~O_NOFOLLOW);
    } else if (native_launch && *path == '/' && !is_work_path(path)) {
        const char *input = !strcmp(path, "/rufus") ? native_executable : path;
        if (disposition != 3 || access & 0x40000000u) {
            last_error = 5;
            return (uintptr_t)-1;
        }
        fd = open(input, mode | O_NONBLOCK);
        struct stat info;
        if (fd >= 0 && (fstat(fd, &info) || !S_ISREG(info.st_mode))) {
            close(fd);
            last_error = 5;
            return (uintptr_t)-1;
        }
    } else {
        fd = open(path, mode, 0600);
    }
#endif
    if (fd >= 0)
        return fd_handle(fd);
    last_error = errno == EACCES || errno == EROFS ? 5 : 2;
    return (uintptr_t)-1;
}

static DWORD copy_path(const char *source, WCHAR *destination, DWORD capacity)
{
    size_t length = strlen(source);
    if (!destination || length >= capacity)
        return length + 1;
    for (size_t i = 0; i <= length; ++i)
        destination[i] = source[i] == '/' ? '\\' : (unsigned char)source[i];
    return length;
}

WIN DWORD win_GetTempPathW(DWORD capacity, WCHAR *destination)
{
    return copy_path("Z:/scratch/", destination, capacity);
}
WIN DWORD win_GetCurrentDirectoryW(DWORD capacity, WCHAR *destination)
{
    char cwd[512], windows[520];
    if (!getcwd(cwd, sizeof(cwd))) {
        last_error = 3;
        return 0;
    }
    snprintf(windows, sizeof(windows), "Z:%s", cwd);
    return copy_path(windows, destination, capacity);
}
WIN DWORD win_GetFinalPathNameByHandleW(uintptr_t handle, WCHAR *destination, DWORD capacity, DWORD flags)
{
    (void)flags;
    int fd = handle_fd(handle);
    char link[40], path[512];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    ssize_t count = fd >= 0 ? readlink(link, path, sizeof(path) - 1) : -1;
    if (count < 0) {
        last_error = 6;
        return 0;
    }
    path[count] = 0;
    char windows[530];
    snprintf(windows, sizeof(windows), "//?/Z:%s", !strcmp(path, "/rufus") ? "/rufus.exe" : path);
    return copy_path(windows, destination, capacity);
}

typedef struct Mutex {
    struct Mutex *next;
    pthread_mutex_t lock;
    char *name;
    unsigned references;
} Mutex;
static Mutex *mutexes;

static Mutex *find_mutex(uintptr_t handle)
{
    for (Mutex *mutex = mutexes; mutex; mutex = mutex->next)
        if ((uintptr_t)mutex == handle)
            return mutex;
    return NULL;
}

WIN uintptr_t win_CreateMutexA(void *security, int owned, const char *name)
{
    (void)security;
    if (name) {
        for (Mutex *mutex = mutexes; mutex; mutex = mutex->next) {
            if (mutex->name && !strcmp(mutex->name, name)) {
                ++mutex->references;
                last_error = 183;
                return (uintptr_t)mutex;
            }
        }
    }
    Mutex *mutex = calloc(1, sizeof(*mutex));
    if (!mutex)
        return 0;
    pthread_mutexattr_t attributes;
    if (pthread_mutexattr_init(&attributes)) {
        free(mutex);
        return 0;
    }
    int error = pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
    if (!error)
        error = pthread_mutex_init(&mutex->lock, &attributes);
    pthread_mutexattr_destroy(&attributes);
    if (error) {
        free(mutex);
        return 0;
    }
    if (name)
        mutex->name = strdup(name);
    mutex->next = mutexes;
    mutex->references = 1;
    mutexes = mutex;
    if (owned)
        pthread_mutex_lock(&mutex->lock);
    last_error = 0;
    return (uintptr_t)mutex;
}

WIN int win_ReleaseMutex(uintptr_t handle)
{
    Mutex *mutex = find_mutex(handle);
    if (mutex && pthread_mutex_unlock(&mutex->lock) == 0)
        return 1;
    last_error = mutex ? 288 : 6;
    return 0;
}

#include "threads.inc"

WIN DWORD win_WaitForSingleObject(uintptr_t handle, DWORD timeout)
{
    GuestThread *thread = find_thread(handle);
    if (thread)
        return wait_thread(thread, timeout);
    GuestEvent *event = find_event(handle);
    if (event)
        return wait_event(event, timeout);
    Mutex *mutex = find_mutex(handle);
    if (mutex) {
        struct timespec end = deadline(timeout);
        int result = timeout == UINT32_MAX ? pthread_mutex_lock(&mutex->lock)
            : pthread_mutex_timedlock(&mutex->lock, &end);
        return result ? 258 : 0;
    }
    last_error = 6;
    return UINT32_MAX;
}

WIN int win_CloseHandle(uintptr_t handle)
{
    if (handle == 0x30001)
        return 1;
    if (find_thread(handle))
        return 1;
    GuestEvent *event = find_event(handle);
    if (event) {
        pthread_mutex_lock(&event->lock);
        event->closed = 1;
        pthread_cond_broadcast(&event->condition);
        pthread_mutex_unlock(&event->lock);
        return 1;
    }
    Mutex *mutex = find_mutex(handle);
    if (mutex) {
        if (mutex->references)
            --mutex->references;
        // Keep the object alive while a thread may still own it.
        return 1;
    }
    int fd = handle_fd(handle);
    if (fd >= 3 && !close(fd))
        return 1;
    last_error = 6;
    return 0;
}

typedef struct {
    uintptr_t internal, bytes;
    uint64_t offset;
    uintptr_t event;
} Overlapped;

static int disk_handle_stat(uintptr_t handle, struct stat *disk);
static int discard_stale_gpt(int fd, uint64_t disk_size, const unsigned char *sector);

static int file_transfer(uintptr_t handle, void *buffer, DWORD count, DWORD *completed,
                         Overlapped *overlapped, int writing)
{
    int fd = handle_fd(handle);
    if (completed)
        *completed = 0;
    if (fd < 0 || (!completed && !overlapped) ||
        (overlapped && overlapped->offset > INT64_MAX)) {
        last_error = 87;
        return 0;
    }
    struct stat disk;
    int replaces_sector_zero = 0;
    if (writing && disk_handle_stat(handle, &disk)) {
        off_t offset = overlapped ? (off_t)overlapped->offset : lseek(fd, 0, SEEK_CUR);
        if (offset < 0 || offset > disk.st_size || count > (uint64_t)(disk.st_size - offset)) {
            last_error = 112;
            return 0;
        }
        replaces_sector_zero = offset == 0 && count >= 512;
    }
    ssize_t result;
#ifdef MOST_ELTORITO
    // The guest consumes fixed-size sectors and assumes successful I/O
    // filled the request. Never let a truncated ISO expose uninitialized data.
    size_t done = 0;
    while (done < count) {
        char *next = (char *)buffer + done;
        if (overlapped) {
            if (done > INT64_MAX - overlapped->offset) {
                last_error = 87;
                return 0;
            }
            off_t offset = overlapped->offset + done;
            result = writing ? pwrite(fd, next, count - done, offset)
                             : pread(fd, next, count - done, offset);
        } else {
            result = writing ? write(fd, next, count - done) : read(fd, next, count - done);
        }
        if (result < 0 && errno == EINTR)
            continue;
        if (result <= 0) {
            last_error = !result && !writing ? 38 : writing ? 29 : 30;
            return 0;
        }
        done += result;
    }
    result = done;
#else
    do {
        if (overlapped)
            result = writing ? pwrite(fd, buffer, count, overlapped->offset)
                             : pread(fd, buffer, count, overlapped->offset);
        else
            result = writing ? write(fd, buffer, count) : read(fd, buffer, count);
    } while (result < 0 && errno == EINTR);
#endif
    if (result < 0) {
        last_error = writing ? 29 : 30;
        if (overlapped) {
            overlapped->internal = 0xc0000001u;
            overlapped->bytes = 0;
        }
        return 0;
    }
    if (replaces_sector_zero && result >= 512 &&
        !discard_stale_gpt(fd, disk.st_size, buffer)) {
        last_error = 29;
        return 0;
    }
    if (completed)
        *completed = result;
    if (overlapped) {
        overlapped->internal = 0;
        overlapped->bytes = result;
        if (overlapped->event)
            win_SetEvent(overlapped->event & ~(uintptr_t)1);
    }
    return 1;
}

WIN int win_ReadFile(uintptr_t handle, void *buffer, DWORD count, DWORD *completed, Overlapped *overlapped)
{
    return file_transfer(handle, buffer, count, completed, overlapped, 0);
}
WIN int win_WriteFile(uintptr_t handle, const void *buffer, DWORD count,
                      DWORD *completed, Overlapped *overlapped)
{
    return file_transfer(handle, (void *)buffer, count, completed, overlapped, 1);
}

WIN void win_Sleep(DWORD ms)
{
    struct timespec interval = {ms / 1000, (ms % 1000) * 1000000};
    while (nanosleep(&interval, &interval) != 0 && errno == EINTR) {}
}

WIN void win_OutputDebugStringA(const char *message)
{
    if (message)
        fprintf(stderr, "%s", message);
}

WIN void win_OutputDebugStringW(const WCHAR *message)
{
    if (!message)
        return;
    size_t chars = 0;
    while (message[chars] && chars < 4096)
        ++chars;
    char utf8[16385];
    int n = transcode("UTF-16LE", "UTF-8", message, chars * 2, utf8, sizeof(utf8) - 1, 1);
    if (n > 0) {
        utf8[n] = 0;
        win_OutputDebugStringA(utf8);
    }
}

static const char *system_message(DWORD code)
{
    switch (code) {
    case 0: return "The operation completed successfully.\r\n";
    case 2: return "The system cannot find the file specified.\r\n";
    case 5: return "Access is denied.\r\n";
    case 6: return "The handle is invalid.\r\n";
    case 8: return "Not enough memory resources are available to process this command.\r\n";
    case 38: return "Reached the end of the file.\r\n";
    case 87: return "The parameter is incorrect.\r\n";
    case 122: return "The data area passed to a system call is too small.\r\n";
    case 126: return "The specified module could not be found.\r\n";
    case 127: return "The specified procedure could not be found.\r\n";
    default: return NULL;
    }
}
WIN DWORD win_FormatMessageW(DWORD flags, const void *source, DWORD code, DWORD language,
                             WCHAR *output, DWORD capacity, const void *arguments)
{
    (void)source; (void)language; (void)arguments;
    const char *message = (flags & 0x1000) ? system_message(code) : NULL;
    if (!message || !output) {
        last_error = 317;
        return 0;
    }
    size_t length = strlen(message);
    WCHAR *target = output;
    if (flags & 0x100) {
        size_t chars = capacity > length ? capacity : length + 1;
        target = calloc(chars, sizeof(WCHAR));
        if (!target) {
            last_error = 8;
            return 0;
        }
        *(WCHAR **)output = target;
    } else if (capacity <= length) {
        last_error = 122;
        return 0;
    }
    for (size_t i = 0; i <= length; ++i)
        target[i] = message[i];
    return length;
}
#if defined(MOST_W32VER) || defined(MOST_RAWCOPY) || defined(MOST_CHSIZE) || defined(MOST_FILEINFO) || defined(MOST_REPTXT) || defined(MOST_ELTORITO) || defined(MOST_EXELIST)
static int format_message_inserts(FILE *stream, const char *source, const uintptr_t *values, int ignore_inserts)
{
    while (*source) {
        if (*source != '%') {
            fputc(*source++, stream);
            continue;
        }
        ++source;
        if (*source == 'n') {
            fputs("\r\n", stream);
            ++source;
            continue;
        }
        if (*source == '%') {
            fputc(*source++, stream);
            continue;
        }
        if (*source == '0')
            break;
        if (ignore_inserts && *source >= '1' && *source <= '9') {
            fputc('%', stream);
            continue;
        }
        if (!values || *source < '1' || *source > '9')
            return 0;
        unsigned index = *source++ - '0';
        if (*source >= '0' && *source <= '9')
            index = index * 10 + (*source++ - '0');
        const uintptr_t value = values[index - 1];
        if (*source != '!') {
            fputs(value ? (const char *)value : "(null)", stream);
        } else if (!strncmp(source, "!u!", 3)) {
            fprintf(stream, "%u", (unsigned)value);
            source += 3;
        } else if (!strncmp(source, "!#x!", 4)) {
            fprintf(stream, "%#x", (unsigned)value);
            source += 4;
        } else {
            return 0;
        }
    }
    return 1;
}

WIN DWORD win_FormatMessageA(DWORD flags, const void *source, DWORD code, DWORD language,
                             char *output, DWORD capacity, const void *arguments)
{
    (void)language;
    const char *message = (flags & 0x400) ? source :
                          (flags & 0x1000) ? system_message(code) : NULL;
    if (!message || !output) {
        last_error = 317;
        return 0;
    }
    char *formatted = NULL;
    size_t length = 0;
    FILE *stream = open_memstream(&formatted, &length);
    if (!stream) {
        last_error = 8;
        return 0;
    }
    const uintptr_t *values = arguments;
    if (values && !(flags & 0x2000))
        memcpy(&values, arguments, sizeof(values));
    int valid = format_message_inserts(stream, message, values, flags & 0x200);
    if (fclose(stream) || !valid || length >= 65536) {
        free(formatted);
        last_error = 87;
        return 0;
    }
    if (flags & 0x100) {
        if (capacity > length + 1) {
            char *larger = realloc(formatted, capacity);
            if (!larger) {
                free(formatted);
                last_error = 8;
                return 0;
            }
            formatted = larger;
        }
        *(char **)output = formatted;
    } else {
        if (capacity <= length) {
            free(formatted);
            last_error = 122;
            return 0;
        }
        memcpy(output, formatted, length + 1);
        free(formatted);
    }
    return length;
}

#endif
#ifdef MOST_W32VER
WIN int win_RtlGetVersion(DWORD *version)
{
    if (!version || (version[0] != 276 && version[0] != 284))
        return (int)0xc000000d;
    DWORD size = version[0];
    memset(version + 1, 0, size - sizeof(DWORD));
    version[1] = 10;
    version[3] = 19045;
    version[4] = 2;
    if (size == 284)
        ((unsigned char *)version)[282] = 1;
    return 0;
}
#endif
WIN void *win_LocalFree(void *allocation) { free(allocation); return NULL; }

WIN DWORD win_GetSystemDirectoryW(WCHAR *destination, DWORD capacity)
{
    return copy_path("Z:/Windows/System32", destination, capacity);
}
static DWORD file_attributes(mode_t mode)
{
    DWORD attributes = S_ISDIR(mode) ? 0x10 : 0;
#ifdef MOST_FILEINFO
    if (!(mode & 0222))
        attributes |= 1;
#endif
    return attributes ? attributes : 0x80;
}

WIN DWORD win_GetFileAttributesW(const WCHAR *name)
{
    char path[512];
    struct stat info;
    if (!host_path(name, path, sizeof(path)) || stat(path, &info) != 0) {
        last_error = 2;
        return UINT32_MAX;
    }
    return file_attributes(info.st_mode);
}
WIN int win_PathFileExistsW(const WCHAR *name)
{
    return win_GetFileAttributesW(name) != UINT32_MAX;
}
WIN int win_AttachConsole(DWORD pid)
{
    if (pid == UINT32_MAX && (isatty(0) || isatty(1) || isatty(2)))
        return 1;
    last_error = 6;
    return 0;
}
WIN DWORD win_GetFullPathNameW(const WCHAR *name, DWORD capacity, WCHAR *buffer, WCHAR **file)
{
    char path[512];
    if (!host_path(name, path, sizeof(path))) {
        last_error = 123;
        return 0;
    }
    char windows[530];
    snprintf(windows, sizeof(windows), "Z:%s", !strcmp(path, "/rufus") ? "/rufus.exe" : path);
    DWORD result = copy_path(windows, buffer, capacity);
    if (file && result < capacity) {
        const char *slash = strrchr(windows, '/');
        *file = slash && slash[1] ? buffer + (slash + 1 - windows) : NULL;
    }
    return result;
}

WIN int win_SHGetSpecialFolderPathW(void *parent, WCHAR *destination, int folder, int create)
{
    (void)parent;
    const char *path;
    switch (folder & 0xff) {
    case 0x1a: path = "/scratch/AppData"; break;
    case 0x1c: path = "/scratch/LocalAppData"; break;
    case 0x23: path = "/scratch/ProgramData"; break;
    case 0x05: path = "/scratch/Documents"; break;
    default: last_error = 2; return 0;
    }
    char local[512];
    if (native_launch) {
        snprintf(local, sizeof(local), "%s%s", native_workdir, path + sizeof("/scratch") - 1);
        path = local;
    }
    if (create && mkdir(path, 0700) != 0 && errno != EEXIST) {
        last_error = 5;
        return 0;
    }
    char windows[100];
    snprintf(windows, sizeof(windows), "Z:%s", path);
    return copy_path(windows, destination, 260) < 260;
}

WIN int win_CreateDirectoryW(const WCHAR *name, void *security)
{
    (void)security;
    char path[512];
    if (!host_path(name, path, sizeof(path))) {
        last_error = 123;
        return 0;
    }
    if (mkdir(path, 0700) == 0)
        return 1;
    last_error = errno == EEXIST ? 183 : errno == ENOENT ? 3 : 5;
    return 0;
}

typedef struct {
    DWORD size, major, minor, build, platform;
    char service_pack[128];
    uint16_t service_pack_major, service_pack_minor, suite;
    uint8_t product, reserved;
} VersionInfoA;

WIN int win_GetVersionExA(VersionInfoA *version)
{
    if (!version || (version->size != 148 && version->size != 156)) {
        last_error = 87;
        return 0;
    }
    DWORD size = version->size;
    memset(version, 0, size);
    version->size = size;
    version->major = 10;
    version->build = 19045;
    version->platform = 2;
    if (size == 156)
        version->product = 1;
    return 1;
}

static int guest_argc;
static WCHAR **guest_argv;

void configure_arguments(int argc, char **argv)
{
    guest_argc = argc;
    guest_argv = calloc((size_t)argc + 1, sizeof(WCHAR *));
    if (!guest_argv)
        abort();
    for (int i = 0; i < argc; ++i) {
        const char *argument = i == 0 ? GUEST_DOS_PATH : argv[i];
#ifdef MOST_RAWCOPY
        int drive_prefix = *argument == '/';
#else
        int drive_prefix = 0;
#endif
        int count = win_MultiByteToWideChar(65001, 0, argument, -1, NULL, 0);
        if (!count)
            abort();
        guest_argv[i] = calloc((size_t)count + 2 * drive_prefix, sizeof(WCHAR));
        if (!guest_argv[i] ||
            !win_MultiByteToWideChar(65001, 0, argument, -1, guest_argv[i] + 2 * drive_prefix, count))
            abort();
        if (drive_prefix) {
            guest_argv[i][0] = 'Z';
            guest_argv[i][1] = ':';
        }
    }
}

WIN int win___wgetmainargs(int *argc, WCHAR ***argv, WCHAR ***env, int wildcard, void *startup)
{
    (void)wildcard; (void)startup;
    static WCHAR *empty_environment[] = { NULL };
    *argc = guest_argc;
    *argv = guest_argv;
    *env = empty_environment;
    return 0;
}

WIN int win_CryptQueryObject(DWORD kind, const void *object, DWORD content, DWORD format, DWORD flags,
                              DWORD *encoding, DWORD *content_type, DWORD *format_type,
                              void **store, void **message, const void **context)
{
    (void)kind; (void)object; (void)content; (void)format; (void)flags;
    if (encoding) *encoding = 0;
    if (content_type) *content_type = 0;
    if (format_type) *format_type = 0;
    if (store) *store = NULL;
    if (message) *message = NULL;
    if (context) *context = NULL;
    last_error = 0x80092009u; // The relinked ELF has no Authenticode signature.
    return 0;
}

static _Thread_local unsigned com_depth;
static _Thread_local DWORD com_apartment;
WIN int32_t win_CoInitializeEx(void *reserved, DWORD mode)
{
    (void)reserved;
    DWORD apartment = mode & 2;
    if (com_depth && com_apartment != apartment)
        return (int32_t)0x80010106u;
    com_apartment = apartment;
    return com_depth++ ? 1 : 0;
}
WIN void win_CoUninitialize(void) { if (com_depth) --com_depth; }

static int com_security_initialized;
WIN int32_t win_CoInitializeSecurity(void *descriptor, int count, void *services,
                                       void *reserved, DWORD level, DWORD impersonation,
                                       void *auth, DWORD capabilities, void *reserved2)
{
    (void)level; (void)impersonation; (void)capabilities;
    if (com_security_initialized)
        return (int32_t)0x80010119u;
    if (descriptor || count != -1 || services || reserved || auth || reserved2)
        return (int32_t)0x80070057u;
    com_security_initialized = 1; // Default security for an in-process-only apartment.
    return 0;
}

extern int32_t create_file_dialog(const void *class_id, const void *interface_id, void **result);

WIN int32_t win_CoCreateInstance(const void *class_id, void *outer, DWORD context,
                                   const void *interface_id, void **result)
{
    (void)context;
    if (result)
        *result = NULL;
    if (!com_depth)
        return (int32_t)0x800401f0u;
    if (outer)
        return (int32_t)0x80040110u;
    return create_file_dialog(class_id, interface_id, result);
}

WIN DWORD win_GetTempFileNameW(const WCHAR *directory, const WCHAR *prefix, DWORD unique, WCHAR *output)
{
    char folder[512], short_name[4];
    if (!host_path(directory, folder, sizeof(folder)) || !prefix || !output) {
        last_error = 87;
        return 0;
    }
    size_t n = 0;
    while (n < 3 && prefix[n]) {
        if (prefix[n] > 127 || prefix[n] == '/' || prefix[n] == '\\') {
            last_error = 87;
            return 0;
        }
        short_name[n] = prefix[n];
        ++n;
    }
    short_name[n] = 0;
    DWORD index = unique & 0xffffu;
    if (!index)
        index = (DWORD)getpid() % 65535u + 1;
    for (unsigned attempt = 0; attempt < 65535; ++attempt, index = index % 65535u + 1) {
        char path[520];
        const char *slash = folder[strlen(folder) - 1] == '/' ? "" : "/";
        if (snprintf(path, sizeof(path), "%s%s%s%04X.tmp", folder, slash, short_name, index) >= 258) {
            last_error = 206;
            return 0;
        }
        if (!unique) {
            int fd = open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
            if (fd < 0 && errno == EEXIST)
                continue;
            if (fd < 0) {
                last_error = 5;
                return 0;
            }
            close(fd);
        }
        char windows[530];
        snprintf(windows, sizeof(windows), "Z:%s", path);
        copy_path(windows, output, 260);
        return index;
    }
    last_error = 80;
    return 0;
}

WIN int win_DeleteFileW(const WCHAR *name)
{
    char path[512];
    if (host_path(name, path, sizeof(path)) && is_work_path(path) && unlink(path) == 0)
        return 1;
    last_error = errno == EACCES ? 5 : 2;
    return 0;
}

WIN int win_GetFileSizeEx(uintptr_t handle, int64_t *length)
{
    int fd = handle_fd(handle);
    struct stat info;
    if (fd < 0 || !length || fstat(fd, &info) != 0) {
        last_error = 6;
        return 0;
    }
    *length = info.st_size;
    return 1;
}

WIN int win_SetFilePointerEx(uintptr_t handle, int64_t distance, int64_t *result, DWORD origin)
{
    int fd = handle_fd(handle);
    if (fd < 0 || origin > 2) {
        last_error = 87;
        return 0;
    }
    off_t offset = lseek(fd, distance, origin);
    if (offset < 0) {
        last_error = 25;
        return 0;
    }
    if (result)
        *result = offset;
    return 1;
}

WIN int win_IsWow64Process2(uintptr_t process, uint16_t *guest, uint16_t *native)
{
    if (process != UINTPTR_MAX || !guest) {
        last_error = 87;
        return 0;
    }
    *guest = 0;
    if (native)
        *native = 0x8664;
    return 1;
}

WIN uint64_t win_VerSetConditionMask(uint64_t mask, DWORD types, uint8_t operation)
{
    for (unsigned bit = 0; bit < 8; ++bit) {
        if (types & (1u << bit)) {
            uint64_t shift = bit * 3;
            mask = (mask & ~(7ull << shift)) | ((uint64_t)(operation & 7) << shift);
        }
    }
    return mask;
}

static int compare_version(DWORD actual, DWORD requested, unsigned operation)
{
    switch (operation) {
    case 1: return actual == requested;
    case 2: return actual > requested;
    case 3: return actual >= requested;
    case 4: return actual < requested;
    case 5: return actual <= requested;
    case 6: return (actual & requested) == requested;
    case 7: return (actual & requested) != 0;
    default: return 0;
    }
}

static int verify_version(const void *version, DWORD types, uint64_t mask, size_t extra_offset)
{
    if (!version || !types) {
        last_error = 87;
        return 0;
    }
    const unsigned char *fields = version;
    DWORD requested[] = {read_dword(fields + 8), read_dword(fields + 4), read_dword(fields + 12),
        read_dword(fields + 16), read_word(fields + extra_offset + 2), read_word(fields + extra_offset),
        read_word(fields + extra_offset + 4), fields[extra_offset + 6]};
    const DWORD actual[] = {0, 10, 19045, 2, 0, 0, 0x100, 1};
    const unsigned order[] = {1, 0, 5, 4, 2, 3, 6, 7};
    int version_order_decided = 0;
    for (unsigned i = 0; i < 8; ++i) {
        unsigned index = order[i];
        if (!(types & (1u << index)))
            continue;
        unsigned operation = (mask >> (index * 3)) & 7;
        if (i < 4 && operation >= 2 && operation <= 5) {
            if (version_order_decided || actual[index] == requested[index])
                continue;
            version_order_decided = 1;
        }
        if (!compare_version(actual[index], requested[index], operation)) {
            last_error = 1150;
            return 0;
        }
    }
    return 1;
}

WIN int win_VerifyVersionInfoA(const void *version, DWORD types, uint64_t mask)
{
    return verify_version(version, types, mask, 148);
}
WIN int win_VerifyVersionInfoW(const void *version, DWORD types, uint64_t mask)
{
    return verify_version(version, types, mask, 276);
}

WIN int win_GetProductInfo(DWORD major, DWORD minor, DWORD sp_major, DWORD sp_minor, DWORD *product)
{
    (void)minor; (void)sp_major; (void)sp_minor;
    if (!product || major < 6) {
        last_error = 87;
        return 0;
    }
    *product = 0x30; // Professional edition of the virtual Win64 environment.
    return 1;
}

typedef struct {
    uint8_t revision, count, authority[6];
    DWORD sub_authority[8];
} Sid;
WIN int win_AllocateAndInitializeSid(const void *authority, uint8_t count, DWORD first, DWORD second,
                                       DWORD third, DWORD fourth, DWORD fifth, DWORD sixth,
                                       DWORD seventh, DWORD eighth, Sid **output)
{
    if (!authority || !output || count > 8) {
        last_error = 87;
        return 0;
    }
    Sid *sid = calloc(1, sizeof(*sid));
    if (!sid) {
        last_error = 8;
        return 0;
    }
    sid->revision = 1;
    sid->count = count;
    memcpy(sid->authority, authority, 6);
    const DWORD parts[] = {first, second, third, fourth, fifth, sixth, seventh, eighth};
    memcpy(sid->sub_authority, parts, count * sizeof(DWORD));
    *output = sid;
    return 1;
}
WIN void *win_FreeSid(Sid *sid) { free(sid); return NULL; }

/* Report the guest token role without changing the Linux process credentials. */
WIN int win_CheckTokenMembership(void *token, const Sid *sid, int *member)
{
    if (token || !sid || !member) {
        last_error = 87;
        return 0;
    }
    static const uint8_t nt_authority[] = {0, 0, 0, 0, 0, 5};
    *member = sid->revision == 1 && sid->count == 2 &&
        !memcmp(sid->authority, nt_authority, sizeof(nt_authority)) &&
        sid->sub_authority[0] == 32 && sid->sub_authority[1] == 544 && (native_launch || geteuid() == 0);
    return 1;
}

#define PROCESS_TOKEN ((uintptr_t)0x30001)
WIN int32_t win_NtOpenProcessToken(uintptr_t process, DWORD access, uintptr_t *token)
{
    (void)access;
    if (process != UINTPTR_MAX || !token)
        return (int32_t)0xc0000008u;
    *token = PROCESS_TOKEN;
    return 0;
}
WIN int32_t win_NtAdjustPrivilegesToken(uintptr_t token, int disable_all, const void *desired,
                                         DWORD size, void *previous, DWORD *returned)
{
    (void)size; (void)previous;
    if (returned)
        *returned = 0;
    if (token != PROCESS_TOKEN)
        return (int32_t)0xc0000008u;
    if (disable_all || !desired || !read_dword(desired))
        return 0;
    return 0x106; // No Windows system privileges are assigned in the sandbox.
}
WIN int32_t win_NtClose(uintptr_t handle)
{
    return handle == PROCESS_TOKEN || win_CloseHandle(handle) ? 0 : (int32_t)0xc0000008u;
}
WIN uintptr_t win_CreateFileA(const char *name, DWORD access, DWORD share, void *security,
                               DWORD disposition, DWORD flags, uintptr_t template)
{
    WCHAR wide[512];
#if defined(MOST_CHSIZE) || defined(MOST_REPTXT) || defined(MOST_ELTORITO)
    const DWORD page = 65001; // The console startup passes Linux argv in UTF-8.
#else
    const DWORD page = 0;
#endif
    if (!win_MultiByteToWideChar(page, 0, name, -1, wide, 512))
        return (uintptr_t)-1;
    return win_CreateFileW(wide, access, share, security, disposition, flags, template);
}

static const unsigned char *version_resource(const WCHAR *name, DWORD *size)
{
    char path[512];
    if (!host_path(name, path, sizeof(path)) || strcmp(path, "/rufus")) {
        char windows[512];
        if (name && ascii_name(name, windows, sizeof(windows))) {
            const char *filename = strrchr(windows, '\\');
            filename = filename ? filename + 1 : windows;
            if (!strcasecmp(filename, "bcdboot.exe")) {
                // The compatibility host has no bcdboot EX switches.
                static const struct {
                    uint16_t length, value_length, type, key[17];
                    DWORD info[13];
                } no_extensions = {92, 52, 0,
                    {'V','S','_','V','E','R','S','I','O','N','_','I','N','F','O',0,0},
                    {0xfeef04bd, 0x10000}};
                *size = sizeof(no_extensions);
                return (const unsigned char *)&no_extensions;
            }
            fprintf(stderr, "No file version for %s\n", windows);
        }
        last_error = 2;
        return NULL;
    }
    const void *resource = win_FindResourceA(NULL, (const char *)1, (const char *)16);
    const unsigned char *data = win_LoadResource(NULL, resource);
    if (!data)
        return NULL;
    *size = win_SizeofResource(NULL, resource);
    return data;
}
WIN DWORD win_GetFileVersionInfoSizeW(const WCHAR *name, DWORD *handle)
{
    DWORD size = 0;
    if (handle)
        *handle = 0;
    return version_resource(name, &size) ? size : 0;
}
WIN int win_GetFileVersionInfoW(const WCHAR *name, DWORD handle, DWORD length, void *output)
{
    (void)handle;
    DWORD size = 0;
    const void *data = version_resource(name, &size);
    if (!data || !output)
        return 0;
    memcpy(output, data, length < size ? length : size);
    return 1;
}
WIN int win_VerQueryValueA(void *block, const char *subblock, void **value, unsigned *length)
{
    if (!block || !subblock || !value || !length || strcmp(subblock, "\\"))
        return 0;
    const unsigned char *start = block;
    uint16_t total = read_word(start);
    uint16_t size = read_word(start + 2);
    size_t cursor = 6;
    while (cursor + 2 <= total && read_word(start + cursor))
        cursor += 2;
    cursor = (cursor + 2 + 3) & ~(size_t)3;
    if (cursor > total || size > total - cursor)
        return 0;
    *value = (void *)(start + cursor);
    *length = size;
    return 1;
}

static uint64_t filetime(struct timespec time)
{
    return 116444736000000000ull + (uint64_t)time.tv_sec * 10000000 + time.tv_nsec / 100;
}
typedef struct {
    DWORD attributes;
    DWORD creation[2], access[2], write[2];
    DWORD high_size, low_size;
} FileAttributeData;

WIN int win_GetFileAttributesExW(const WCHAR *name, int level, FileAttributeData *data)
{
    char path[512];
    struct stat info;
    if (level || !data) {
        last_error = 87;
        return 0;
    }
    if (!host_path(name, path, sizeof(path)) || stat(path, &info) != 0) {
        last_error = 2;
        return 0;
    }
    uint64_t creation = filetime(info.st_ctim), access = filetime(info.st_atim),
             write = filetime(info.st_mtim), size = info.st_size;
    *data = (FileAttributeData){S_ISDIR(info.st_mode) ? 0x10 : 0x80,
        {creation, creation >> 32}, {access, access >> 32}, {write, write >> 32}, size >> 32, size};
    return 1;
}

WIN uint64_t win_GetTickCount64(void)
{
    return clock_100ns(CLOCK_BOOTTIME) / 10000;
}
WIN DWORD win_GetTickCount(void) { return (DWORD)win_GetTickCount64(); }

WIN int win_MulDiv(int a, int b, int divisor)
{
    if (!divisor)
        return -1;
    int64_t product = (int64_t)a * b;
    int64_t denominator = divisor;
    if (denominator < 0) {
        denominator = -denominator;
        product = -product;
    }
    int64_t result = (product + (product >= 0 ? denominator / 2 : -denominator / 2)) / denominator;
    return result < INT_MIN || result > INT_MAX ? -1 : (int)result;
}

WIN int win_LCIDToLocaleName(DWORD locale, WCHAR *destination, int capacity, DWORD flags)
{
    (void)flags;
    const char *name;
    switch (locale) {
    case 0x0409: case 0x0400: case 0x0800: name = "en-US"; break;
    case 0x0419: name = "ru-RU"; break;
    case 0x007f: name = ""; break;
    default: last_error = 87; return 0;
    }
    int needed = strlen(name) + 1;
    if (capacity == 0)
        return needed;
    if (capacity < needed || !destination) {
        last_error = 122;
        return 0;
    }
    for (int i = 0; i < needed; ++i)
        destination[i] = name[i];
    return needed;
}

typedef struct DeviceSet {
    struct DeviceSet *next;
    unsigned char device_class[16];
    DWORD flags;
    int present;
} DeviceSet;
static DeviceSet *device_sets;
static const unsigned char disk_interface_id[16] =
    {0x07,0x63,0xf5,0x53,0xbf,0xb6,0xd0,0x11,0x94,0xf2,0,0xa0,0xc9,0x1e,0xfb,0x8b};

typedef struct {
    DWORD size;
    unsigned char class_id[16];
    DWORD state;
    uintptr_t reserved;
} DeviceEntry;
_Static_assert(sizeof(DeviceEntry) == 32, "Win64 device entry ABI");

static DeviceSet **device_set_handle(void *handle)
{
    DeviceSet **link = &device_sets;
    while (*link && *link != handle)
        link = &(*link)->next;
    return link;
}

WIN void *win_SetupDiGetClassDevsA(const void *class_id, const char *enumerator, void *parent, DWORD flags)
{
    (void)enumerator; (void)parent;
    DeviceSet *set = calloc(1, sizeof(*set));
    if (!set) {
        last_error = 8;
        return (void *)-1;
    }
    set->flags = flags;
    if (class_id)
        memcpy(set->device_class, class_id, sizeof(set->device_class));
    struct stat disk;
    set->present = !memcmp(set->device_class, disk_interface_id, 16) && probe_disk_stat(&disk);
    set->next = device_sets;
    device_sets = set;
    return set;
}
static int device_entry(DeviceSet *set, DWORD index, DeviceEntry *entry, DWORD state)
{
    if (!set || !entry || entry->size != sizeof(*entry)) {
        last_error = set ? 87 : 6;
        return 0;
    }
    if (index || !set->present) {
        last_error = 259;
        return 0;
    }
    memcpy(entry->class_id, set->device_class, 16);
    entry->state = state;
    entry->reserved = (uintptr_t)set;
    return 1;
}
WIN int win_SetupDiEnumDeviceInterfaces(void *handle, const void *device, const void *interface_id,
                                        DWORD index, DeviceEntry *entry)
{
    (void)device; (void)interface_id;
    return device_entry(*device_set_handle(handle), index, entry, 1);
}
WIN int win_SetupDiEnumDeviceInfo(void *handle, DWORD index, DeviceEntry *entry)
{
    return device_entry(*device_set_handle(handle), index, entry, 2);
}
WIN int win_SetupDiGetDeviceInterfaceDetailA(void *handle, const DeviceEntry *interface,
                                            void *detail, DWORD size, DWORD *needed, DeviceEntry *device)
{
    DeviceSet *set = *device_set_handle(handle);
    if (!set || !interface || interface->reserved != (uintptr_t)set) {
        last_error = 87;
        return 0;
    }
    const DWORD length = 4 + sizeof(probe_disk_link);
    if (needed)
        *needed = length;
    if (!detail || size < length) {
        last_error = 122;
        return 0;
    }
    if (read_dword(detail) != 8) {
        last_error = 1784;
        return 0;
    }
    if (device && !device_entry(set, 0, device, 2))
        return 0;
    memcpy((char *)detail + 4, probe_disk_link, sizeof(probe_disk_link));
    return 1;
}
WIN int win_SetupDiGetDeviceInstanceIdA(void *handle, const DeviceEntry *entry, char *output,
                                        DWORD capacity, DWORD *needed)
{
    DeviceSet *set = *device_set_handle(handle);
    if (!set || !entry || entry->reserved != (uintptr_t)set) {
        last_error = 87;
        return 0;
    }
    if (needed)
        *needed = sizeof(probe_disk_instance);
    if (!output || capacity < sizeof(probe_disk_instance)) {
        last_error = 122;
        return 0;
    }
    memcpy(output, probe_disk_instance, sizeof(probe_disk_instance));
    return 1;
}
WIN int win_SetupDiDestroyDeviceInfoList(void *handle)
{
    DeviceSet **link = device_set_handle(handle);
    if (!*link) {
        last_error = 6;
        return 0;
    }
    DeviceSet *set = *link;
    *link = set->next;
    free(set);
    return 1;
}

static DWORD device_id_list_size(const char *filter)
{
    struct stat disk;
    return filter && !strcasecmp(filter, "USBSTOR") && probe_disk_stat(&disk) ?
        sizeof(probe_disk_instance) + 1 : 2;
}
WIN DWORD win_CM_Get_Device_ID_List_SizeA(DWORD *length, const char *filter, DWORD flags)
{
    (void)flags;
    if (!length)
        return 3;
    *length = device_id_list_size(filter);
    return 0;
}
WIN DWORD win_CM_Get_Device_ID_ListA(const char *filter, char *buffer, DWORD length, DWORD flags)
{
    (void)flags;
    if (!buffer)
        return 3;
    DWORD needed = device_id_list_size(filter);
    if (length < needed)
        return 0x1a;
    memset(buffer, 0, needed);
    if (needed > 2)
        memcpy(buffer, probe_disk_instance, sizeof(probe_disk_instance));
    return 0;
}
WIN DWORD win_GetLogicalDrives(void) { return (1u << 25) | (probe_mounted ? 1u << 2 : 0); }
WIN DWORD win_GetDriveTypeW(const WCHAR *name)
{
    if (!name)
        return 3;
    if (probe_mounted && (name[0] == 'C' || name[0] == 'c') && name[1] == ':')
        return 2;
    return ((name[0] == 'Z' || name[0] == 'z') && name[1] == ':') ? 3 : 1;
}
WIN DWORD win_GetDriveTypeA(const char *name)
{
    if (!name)
        return 3;
    if (is_probe_volume(name) && probe_volume_ready())
        return 2;
    if (probe_mounted && (name[0] == 'C' || name[0] == 'c') && name[1] == ':')
        return 2;
    return ((name[0] == 'Z' || name[0] == 'z') && name[1] == ':') ? 3 : 1;
}
WIN DWORD win_GetLogicalDriveStringsW(DWORD capacity, WCHAR *buffer)
{
    static const WCHAR all[] = {'C', ':', '\\', 0, 'Z', ':', '\\', 0, 0};
    DWORD count = probe_mounted ? 9 : 5;
    if (capacity < count || !buffer)
        return count;
    memcpy(buffer, all + 9 - count, count * sizeof(WCHAR));
    return count - 1;
}

WIN int32_t win_SHGetSpecialFolderLocation(void *parent, int folder, void **pidl)
{
    (void)parent;
    if (!pidl)
        return (int32_t)0x80070057u;
    *pidl = NULL;
    if (folder != 0)
        return (int32_t)0x80070002u;
    *pidl = calloc(1, 2); // An empty item list names the desktop namespace root.
    return *pidl ? 0 : (int32_t)0x8007000eu;
}
WIN void win_CoTaskMemFree(void *value) { free(value); }

WIN DWORD win_ordinal_2(void *window, int sources, int32_t events,
                                             unsigned message, int count, const void *entries)
{
    (void)window; (void)sources; (void)events; (void)message; (void)count; (void)entries;
    last_error = 50;
    return 0; // The isolated namespace does not expose a shell change-notification source.
}

WIN int win_SetCurrentDirectoryW(const WCHAR *name)
{
    char path[512];
    if (!host_path(name, path, sizeof(path)) || !is_work_path(path)) {
        last_error = 3;
        return 0;
    }
    if (chdir(path) != 0) {
        last_error = errno == EACCES ? 5 : 3;
        return 0;
    }
    return 1;
}

WIN int win_OpenProcessToken(uintptr_t process, DWORD access, uintptr_t *token)
{
    if (!win_NtOpenProcessToken(process, access, token))
        return 1;
    last_error = 6;
    return 0;
}

WIN int win_GetTokenInformation(uintptr_t token, int kind, void *output, DWORD capacity, DWORD *required)
{
    if (token != PROCESS_TOKEN || !required) {
        last_error = 6;
        return 0;
    }
    DWORD needed;
    if (kind == 1)
        needed = 16 + 8 + 5 * sizeof(DWORD);
    else if (kind == 18 || kind == 20)
        needed = sizeof(DWORD);
    else {
        last_error = 87;
        return 0;
    }
    *required = needed;
    if (!output || capacity < needed) {
        last_error = 122;
        return 0;
    }
    memset(output, 0, needed);
    if (kind == 1) {
        Sid *sid = (Sid *)((char *)output + 16);
        *(Sid **)output = sid;
        sid->revision = 1;
        sid->authority[5] = 5;
        sid->count = 5;
        const DWORD identity[] = {21, 1, 1, 1, (DWORD)getuid() + 1000};
        memcpy(sid->sub_authority, identity, sizeof(identity));
    } else {
        *(DWORD *)output = kind == 18 ? 1 : (native_launch || geteuid() == 0);
    }
    return 1;
}
WIN int win_ConvertSidToStringSidA(const Sid *sid, char **output)
{
    if (!sid || !output || sid->revision != 1 || sid->count > 8) {
        last_error = 1337;
        return 0;
    }
    uint64_t authority = 0;
    for (size_t i = 0; i < sizeof(sid->authority); ++i)
        authority = (authority << 8) | sid->authority[i];
    char buffer[128];
    int n = snprintf(buffer, sizeof(buffer), "S-%u-%lu", sid->revision, authority);
    for (unsigned i = 0; i < sid->count; ++i)
        n += snprintf(buffer + n, sizeof(buffer) - n, "-%u", sid->sub_authority[i]);
    *output = strdup(buffer);
    if (!*output) {
        last_error = 8;
        return 0;
    }
    return 1;
}

WIN int win_ConvertStringSidToSidA(const char *text, Sid **output)
{
    if (!text || !output || strncasecmp(text, "S-1-", 4)) {
        last_error = 1337;
        return 0;
    }
    *output = NULL;
    const char *cursor = text + 4;
    char *end;
    errno = 0;
    uint64_t authority = strtoull(cursor, &end, 10);
    if (errno || end == cursor || authority > 0xffffffffffffull) {
        last_error = 1337;
        return 0;
    }
    Sid sid = {.revision = 1};
    for (int i = 5; i >= 0; --i, authority >>= 8)
        sid.authority[i] = authority;
    while (*end) {
        cursor = end + 1;
        if (*end != '-' || sid.count == 8 || *cursor < '0' || *cursor > '9') {
            last_error = 1337;
            return 0;
        }
        unsigned long part = strtoul(cursor, &end, 10);
        if (errno || part > UINT32_MAX) {
            last_error = 1337;
            return 0;
        }
        sid.sub_authority[sid.count++] = part;
    }
    *output = malloc(sizeof(sid));
    if (!*output) {
        last_error = 8;
        return 0;
    }
    **output = sid;
    return 1;
}

typedef struct {
    uint8_t revision, reserved;
    uint16_t control;
    void *owner, *group, *sacl, *dacl;
} SecurityDescriptor;

WIN int win_InitializeSecurityDescriptor(SecurityDescriptor *descriptor, DWORD revision)
{
    if (!descriptor || revision != 1) {
        last_error = 1305;
        return 0;
    }
    memset(descriptor, 0, sizeof(*descriptor));
    descriptor->revision = 1;
    return 1;
}
WIN int win_SetSecurityDescriptorOwner(SecurityDescriptor *descriptor, Sid *owner, int defaulted)
{
    if (!descriptor || descriptor->revision != 1 || (descriptor->control & 0x8000)) {
        last_error = 1338;
        return 0;
    }
    descriptor->owner = owner;
    descriptor->control = (descriptor->control & ~1u) | (defaulted ? 1u : 0u);
    return 1;
}

WIN int32_t win_SHGetKnownFolderPath(const unsigned char *id, DWORD flags, void *token, WCHAR **output)
{
    (void)flags; (void)token;
    if (!id || !output)
        return (int32_t)0x80070057u;
    *output = NULL;
    static const unsigned char downloads[16] =
        {0x90,0xe2,0x4d,0x37,0x3f,0x12,0x65,0x45,0x91,0x64,0x39,0xc4,0x92,0x5e,0x46,0x7b};
    static const unsigned char documents[16] =
        {0xd3,0x09,0xad,0xfd,0x8f,0xc5,0xaf,0x4a,0x8f,0x7d,0xb0,0x3e,0x33,0xb2,0x5c,0x4f};
    static const unsigned char desktop[16] =
        {0x3a,0xcc,0xbf,0xb4,0x2c,0xdb,0x4c,0x42,0xb0,0x29,0x7f,0xe9,0x9a,0x87,0xc6,0x41};
    const char *path;
    if (!memcmp(id, downloads, 16))
        path = "Z:/scratch";
    else if (!memcmp(id, documents, 16))
        path = "Z:/scratch/Documents";
    else if (!memcmp(id, desktop, 16))
        path = "Z:/scratch/Desktop";
    else
        return (int32_t)0x80070002u;
    size_t capacity = strlen(path) + 1;
    *output = calloc(capacity, sizeof(WCHAR));
    if (!*output)
        return (int32_t)0x8007000eu;
    copy_path(path, *output, capacity);
    return 0;
}

typedef struct {
    DWORD attributes;
    DWORD creation[2], access[2], write[2];
    DWORD volume_serial, size_high, size_low, links, index_high, index_low;
} FileHandleInformation;

WIN int win_GetFileInformationByHandle(uintptr_t handle, FileHandleInformation *data)
{
    struct stat info;
    int fd = handle_fd(handle);
    if (fd < 0 || !data || fstat(fd, &info) != 0) {
        last_error = 6;
        return 0;
    }
#ifdef MOST_FILEINFO
    struct statx extra;
    uint64_t creation = 0;
    if (!statx(fd, "", AT_EMPTY_PATH, STATX_BTIME, &extra) && (extra.stx_mask & STATX_BTIME))
        creation = filetime((struct timespec){extra.stx_btime.tv_sec, extra.stx_btime.tv_nsec});
#else
    uint64_t creation = filetime(info.st_ctim);
#endif
    uint64_t access = filetime(info.st_atim), write = filetime(info.st_mtim);
    uint64_t size = info.st_size, index = info.st_ino;
    *data = (FileHandleInformation){file_attributes(info.st_mode),
        {creation, creation >> 32}, {access, access >> 32}, {write, write >> 32},
        info.st_dev, size >> 32, size, info.st_nlink, index >> 32, index};
    return 1;
}
WIN int win_GetFileTime(uintptr_t handle, void *creation, void *access, void *write)
{
    FileHandleInformation data;
    if (!win_GetFileInformationByHandle(handle, &data))
        return 0;
    if (creation) memcpy(creation, data.creation, 8);
    if (access) memcpy(access, data.access, 8);
    if (write) memcpy(write, data.write, 8);
    return 1;
}
WIN DWORD win_GetFileSize(uintptr_t handle, DWORD *high)
{
    int64_t length;
    if (!win_GetFileSizeEx(handle, &length))
        return UINT32_MAX;
    if (high)
        *high = (uint64_t)length >> 32;
    if ((DWORD)length == UINT32_MAX)
        last_error = 0;
    return length;
}
WIN DWORD win_SetFilePointer(uintptr_t handle, int32_t low, int32_t *high, DWORD origin)
{
    int64_t distance = high ? (int64_t)(((uint64_t)(DWORD)*high << 32) | (DWORD)low) : low;
    int64_t result;
    if (!win_SetFilePointerEx(handle, distance, &result, origin))
        return UINT32_MAX;
    if (high)
        *high = (uint64_t)result >> 32;
    if ((DWORD)result == UINT32_MAX)
        last_error = 0;
    return result;
}
WIN int win_FlushFileBuffers(uintptr_t handle)
{
    int fd = handle_fd(handle);
    if (fd >= 0 && fsync(fd) == 0)
        return 1;
    last_error = 6;
    return 0;
}
WIN int win_SetEndOfFile(uintptr_t handle)
{
    int fd = handle_fd(handle);
    struct stat disk;
    if (disk_handle_stat(handle, &disk)) {
        last_error = 1;
        return 0;
    }
    off_t at = fd >= 0 ? lseek(fd, 0, SEEK_CUR) : -1;
    if (at >= 0 && ftruncate(fd, at) == 0)
        return 1;
    last_error = 6;
    return 0;
}

typedef struct { uint16_t year, month, weekday, day, hour, minute, second, millisecond; } SystemTime;

static void to_system_time(const struct tm *time, unsigned ms, SystemTime *out)
{
    *out = (SystemTime){time->tm_year + 1900, time->tm_mon + 1, time->tm_wday, time->tm_mday,
        time->tm_hour, time->tm_min, time->tm_sec, ms};
}
WIN int win_FileTimeToSystemTime(const uint64_t *value, SystemTime *out)
{
    if (!value || !out || *value > 0x7fffffffffffffffull) {
        last_error = 87;
        return 0;
    }
#ifdef MOST_FILEINFO
    if (!*value) {
        last_error = 87;
        return 0;
    }
#endif
    time_t seconds = (int64_t)(*value / 10000000) - 11644473600ll;
    struct tm time;
    if (!gmtime_r(&seconds, &time)) {
        last_error = 87;
        return 0;
    }
    to_system_time(&time, *value / 10000 % 1000, out);
    return 1;
}
WIN void win_GetSystemTime(SystemTime *out)
{
    uint64_t value;
    win_GetSystemTimeAsFileTime(&value);
    win_FileTimeToSystemTime(&value, out);
}
WIN void win_GetLocalTime(SystemTime *out)
{
    struct timespec now;
    struct tm time;
    if (clock_gettime(CLOCK_REALTIME, &now) == 0 && localtime_r(&now.tv_sec, &time))
        to_system_time(&time, now.tv_nsec / 1000000, out);
}

static int system_seconds(const SystemTime *system, time_t *result)
{
    if (!system || system->year < 1601 || system->month < 1 || system->month > 12 ||
        system->day < 1 || system->day > 31 || system->hour > 23 || system->minute > 59 ||
        system->second > 59 || system->millisecond > 999)
        return 0;
    struct tm time = {.tm_year = system->year - 1900, .tm_mon = system->month - 1,
        .tm_mday = system->day, .tm_hour = system->hour, .tm_min = system->minute,
        .tm_sec = system->second};
    *result = timegm(&time);
    return time.tm_mday == system->day;
}
WIN int win_SystemTimeToTzSpecificLocalTime(const void *zone, const SystemTime *utc, SystemTime *local)
{
    time_t seconds;
    struct tm time;
    if (zone || !local || !system_seconds(utc, &seconds) || !localtime_r(&seconds, &time)) {
        last_error = 87;
        return 0;
    }
    to_system_time(&time, utc->millisecond, local);
    return 1;
}
WIN int win_SystemTimeToFileTime(const SystemTime *time, uint64_t *value)
{
    time_t seconds;
    if (!value || !system_seconds(time, &seconds) || seconds > 910692730085ll) {
        last_error = 87;
        return 0;
    }
    *value = (uint64_t)(seconds + 11644473600ll) * 10000000 + time->millisecond * 10000ull;
    return 1;
}

typedef struct {
    int32_t bias;
    WCHAR standard_name[32];
    SystemTime standard_date;
    int32_t standard_bias;
    WCHAR daylight_name[32];
    SystemTime daylight_date;
    int32_t daylight_bias;
} TimeZoneInfo;

WIN DWORD win_GetTimeZoneInformation(TimeZoneInfo *out)
{
    if (!out) {
        last_error = 87;
        return UINT32_MAX;
    }
    memset(out, 0, sizeof(*out));
    time_t now = time(NULL);
    struct tm local;
    if (localtime_r(&now, &local)) {
        out->bias = -local.tm_gmtoff / 60;
        const char *name = local.tm_zone ? local.tm_zone : "UTC";
        size_t length = strnlen(name, 31);
        for (size_t i = 0; i < length; ++i)
            out->standard_name[i] = (unsigned char)name[i];
    }
    return 0;
}

WIN int win_SetupDiGetDeviceRegistryPropertyA(void *handle, const DeviceEntry *entry, DWORD property,
                                              DWORD *type, void *output, DWORD capacity, DWORD *needed)
{
    DeviceSet *set = *device_set_handle(handle);
    if (!set || !entry || entry->reserved != (uintptr_t)set) {
        last_error = 87;
        return 0;
    }
    const char *text = NULL;
    DWORD value_type = 1, value = 0, size;
    switch (property) {
    case 0: case 12: text = "Most Test Media USB Device"; break;
    case 1: text = "USBSTOR\\DiskMost____Test_Media______1.00\0"; value_type = 7; break;
    case 4: text = "disk"; break;
    case 11: text = "Most"; break;
    case 15: value = 4; value_type = 4; break;
    case 22: text = "USBSTOR"; break;
    case 31: value = 3; value_type = 4; break;
    default:
        fprintf(stderr, "Unsupported device property %#x\n", property);
        last_error = 13;
        return 0;
    }
    size = text ? strlen(text) + (value_type == 7 ? 2 : 1) : sizeof(value);
    if (type)
        *type = value_type;
    if (needed)
        *needed = size;
    if (!output || capacity < size) {
        last_error = 122;
        return 0;
    }
    memcpy(output, text ? (const void *)text : (const void *)&value, size);
    return 1;
}

WIN int win_SetupDiGetDeviceRegistryPropertyW(void *handle, const DeviceEntry *entry, DWORD property,
                                              DWORD *type, void *output, DWORD capacity, DWORD *needed)
{
    char narrow[256];
    DWORD ansi_size, kind;
    if (!win_SetupDiGetDeviceRegistryPropertyA(handle, entry, property, &kind,
                                               narrow, sizeof(narrow), &ansi_size))
        return 0;
    if (type)
        *type = kind;
    DWORD size = kind == 4 ? ansi_size : ansi_size * 2;
    if (needed)
        *needed = size;
    if (!output || capacity < size) {
        last_error = 122;
        return 0;
    }
    if (kind == 4)
        memcpy(output, narrow, size);
    else if (!win_MultiByteToWideChar(0, 0, narrow, ansi_size, output, capacity / 2))
        return 0;
    return 1;
}

static const char probe_usb_instance[] = "USB\\VID_1209&PID_0001\\MOSTPROBE";
static const char probe_root_instance[] = "HTREE\\ROOT\\0";

WIN DWORD win_CM_Locate_DevNodeA(DWORD *node, const char *id, DWORD flags)
{
    (void)flags;
    if (!node)
        return 3;
    if (!id || !strcasecmp(id, probe_root_instance))
        *node = 1;
    else if (!strcasecmp(id, probe_disk_instance))
        *node = 2;
    else if (!strcasecmp(id, probe_usb_instance))
        *node = 3;
    else
        return 0x0d;
    return 0;
}
WIN DWORD win_CM_Get_Parent(DWORD *parent, DWORD node, DWORD flags)
{
    (void)flags;
    if (!parent)
        return 3;
    if (node == 2)
        *parent = 3;
    else if (node == 3)
        *parent = 1;
    else
        return 0x0d;
    return 0;
}
WIN DWORD win_CM_Get_Child(DWORD *child, DWORD node, DWORD flags)
{
    (void)flags;
    if (!child)
        return 3;
    if (node == 1)
        *child = 3;
    else if (node == 3)
        *child = 2;
    else
        return 0x0d;
    return 0;
}
WIN DWORD win_CM_Get_Sibling(DWORD *sibling, DWORD node, DWORD flags)
{
    (void)sibling; (void)node; (void)flags;
    return 0x0d;
}
WIN DWORD win_CM_Get_Device_IDA(DWORD node, char *output, DWORD length, DWORD flags)
{
    (void)flags;
    const char *id = node == 1 ? probe_root_instance : node == 2 ? probe_disk_instance :
        node == 3 ? probe_usb_instance : NULL;
    if (!id)
        return 0x0d;
    size_t size = strlen(id) + 1;
    if (!output || length < size)
        return 0x1a;
    memcpy(output, id, size);
    return 0;
}

static int disk_handle_stat(uintptr_t handle, struct stat *disk)
{
    int fd = handle_fd(handle);
    struct stat opened;
    return fd >= 0 && probe_disk_stat(disk) && fstat(fd, &opened) == 0 &&
        opened.st_dev == disk->st_dev && opened.st_ino == disk->st_ino;
}

enum { disk_layout_header_size = 48, disk_partition_size = 144,
       mbr_partition_count = 4, disk_layout_limit = 4096,
       gpt_entry_size = 128, gpt_entry_limit = 128 };

static uint64_t disk_qword(const void *bytes)
{
    uint64_t value;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

static DWORD gpt_crc32(const unsigned char *bytes, size_t length)
{
    DWORD crc = UINT32_MAX;
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (crc & 1 ? 0xedb88320u : 0);
    }
    return ~crc;
}

// A confirmed raw overwrite with a non-GPT image replaces sector zero.
// Remove an independently valid backup GPT so Linux cannot recover the old layout.
static int discard_stale_gpt(int fd, uint64_t disk_size, const unsigned char *sector)
{
    if (sector[510] == 0x55 && sector[511] == 0xaa)
        for (unsigned i = 0; i < mbr_partition_count; ++i)
            if (sector[446 + i * 16 + 4] == 0xee)
                return 1;
    uint64_t end = disk_size / 512 - 1;
    unsigned char header[512], table[gpt_entry_limit * gpt_entry_size + 512];
    if (pread(fd, header, sizeof(header), end * 512) != sizeof(header))
        return 0;
    if (memcmp(header, "EFI PART", 8))
        return 1;
    DWORD header_size = read_dword(header + 12), checksum = read_dword(header + 16);
    uint64_t start = disk_qword(header + 72);
    DWORD slots = read_dword(header + 80);
    size_t bytes = (size_t)slots * gpt_entry_size;
    memset(header + 16, 0, 4);
    if (header_size < 92 || header_size > sizeof(header) ||
        gpt_crc32(header, header_size) != checksum ||
        disk_qword(header + 24) != end || disk_qword(header + 32) != 1 ||
        start <= disk_qword(header + 48) || start >= end ||
        !slots || slots > gpt_entry_limit || read_dword(header + 84) != gpt_entry_size ||
        (bytes + 511) / 512 != end - start ||
        pread(fd, table, bytes, start * 512) != (ssize_t)bytes ||
        gpt_crc32(table, bytes) != read_dword(header + 88))
        return 1;
    size_t length = (end - start + 1) * 512;
    memset(table, 0, length);
    ssize_t written;
    do {
        written = pwrite(fd, table, length, start * 512);
    } while (written < 0 && errno == EINTR);
    return written == (ssize_t)length;
}

static unsigned gpt_layout(int fd, uint64_t disk_sectors, unsigned char *output)
{
    unsigned char header[512], entries[gpt_entry_limit * gpt_entry_size];
    if (pread(fd, header, sizeof(header), 512) != sizeof(header))
        return 0;
    DWORD header_size = read_dword(header + 12), header_crc = read_dword(header + 16);
    uint64_t first = disk_qword(header + 40), last = disk_qword(header + 48);
    uint64_t table = disk_qword(header + 72);
    DWORD slots = read_dword(header + 80), stride = read_dword(header + 84);
    size_t bytes = (size_t)slots * gpt_entry_size;
    memset(header + 16, 0, 4);
    if (memcmp(header, "EFI PART", 8) || read_dword(header + 8) != 0x10000 ||
        header_size < 92 || header_size > sizeof(header) ||
        gpt_crc32(header, header_size) != header_crc ||
        disk_qword(header + 24) != 1 || disk_qword(header + 32) != disk_sectors - 1 ||
        first > last || last >= disk_sectors - 1 || table < 2 || table >= first ||
        !slots || slots > gpt_entry_limit || stride != gpt_entry_size ||
        (bytes + 511) / 512 > first - table ||
        pread(fd, entries, bytes, table * 512) != (ssize_t)bytes ||
        gpt_crc32(entries, bytes) != read_dword(header + 88))
        return 0;

    static const unsigned char zero_guid[16];
    DWORD count = 0, style = 1; // PARTITION_STYLE_GPT
    for (DWORD i = 0; i < slots; ++i) {
        const unsigned char *entry = entries + i * gpt_entry_size;
        if (!memcmp(entry, zero_guid, sizeof(zero_guid)))
            continue;
        uint64_t start = disk_qword(entry + 32), end = disk_qword(entry + 40);
        if (start < first || end > last || start > end ||
            count == (disk_layout_limit - disk_layout_header_size) / disk_partition_size)
            return 0;
        unsigned char *partition = output + disk_layout_header_size + count++ * disk_partition_size;
        uint64_t start_bytes = start * 512, length = (end - start + 1) * 512;
        DWORD number = i + 1;
        memcpy(partition, &style, 4);
        memcpy(partition + 8, &start_bytes, 8);
        memcpy(partition + 16, &length, 8);
        memcpy(partition + 24, &number, 4);
        memcpy(partition + 32, entry, 32); // Type and partition GUIDs.
        memcpy(partition + 64, entry + 48, 80); // Attributes and UTF-16 name.
    }
    uint64_t usable_start = first * 512, usable_length = (last - first + 1) * 512;
    memcpy(output, &style, 4);
    memcpy(output + 4, &count, 4);
    memcpy(output + 8, header + 56, 16);
    memcpy(output + 24, &usable_start, 8);
    memcpy(output + 32, &usable_length, 8);
    memcpy(output + 40, &slots, 4);
    return disk_layout_header_size + count * disk_partition_size;
}

static unsigned disk_layout(int fd, uint64_t disk_size, unsigned char *output)
{
    unsigned char sector[512];
    if (pread(fd, sector, sizeof(sector), 0) != sizeof(sector)) {
        last_error = 30;
        return 0;
    }
    DWORD style = 2; // PARTITION_STYLE_RAW, including a FAT superfloppy.
    unsigned number = 0;
    if (sector[510] == 0x55 && sector[511] == 0xaa) {
        for (unsigned i = 0; i < mbr_partition_count; ++i) {
            const unsigned char *entry = sector + 446 + i * 16;
            unsigned char type = entry[4];
            if (type == 0xee) {
                unsigned length = gpt_layout(fd, disk_size / 512, output);
                if (!length) {
                    fprintf(stderr, "Unsupported or invalid primary GPT\n");
                    last_error = 1;
                }
                return length;
            }
            uint64_t first = read_dword(entry + 8), count = read_dword(entry + 12);
            if (!type || !count)
                continue;
            if (!first || first + count > disk_size / 512 ||
                (entry[0] != 0 && entry[0] != 0x80) ||
                type == 0x05 || type == 0x0f || type == 0x85) {
                fprintf(stderr, "Unsupported or invalid MBR partition table\n");
                last_error = 1;
                return 0;
            }
            style = 0; // PARTITION_STYLE_MBR
            unsigned char *partition = output + disk_layout_header_size + i * disk_partition_size;
            uint64_t start = first * 512, length = count * 512;
            DWORD ordinal = ++number, hidden = first;
            memcpy(partition + 8, &start, sizeof(start));
            memcpy(partition + 16, &length, sizeof(length));
            memcpy(partition + 24, &ordinal, sizeof(ordinal));
            partition[32] = type;
            partition[33] = entry[0] == 0x80;
            partition[34] = 1;
            memcpy(partition + 36, &hidden, sizeof(hidden));
        }
    }
    memcpy(output, &style, sizeof(style));
    if (!number)
        return disk_layout_header_size;
    DWORD slots = mbr_partition_count;
    memcpy(output + 4, &slots, sizeof(slots));
    memcpy(output + 8, sector + 440, 4);
    return disk_layout_header_size + mbr_partition_count * disk_partition_size;
}

WIN int win_DeviceIoControl(uintptr_t handle, DWORD code, const void *input, DWORD input_size,
                             void *output, DWORD capacity, DWORD *returned, Overlapped *overlapped)
{
    (void)input; (void)input_size;
    if (returned)
        *returned = 0;
#ifdef MOST_CHSIZE
    if (code == 0x900c4 && !input && !input_size && !overlapped) { // FSCTL_SET_SPARSE
        struct stat file;
        int fd = handle_fd(handle);
        if (fd >= 0 && fstat(fd, &file) == 0 && S_ISREG(file.st_mode))
            return 1; // Linux ftruncate extends regular files with holes where supported.
    }
#endif
    struct stat disk;
    if (!disk_handle_stat(handle, &disk) || overlapped) {
        last_error = 6;
        return 0;
    }
    union {
        uint64_t align;
        unsigned char bytes[disk_layout_limit];
    } info = {.bytes = {0}};
    unsigned length;
    DWORD *words = (DWORD *)info.bytes;
    int64_t *wide = (int64_t *)info.bytes;
    switch (code) {
    case 0x90018: // FSCTL_LOCK_VOLUME
        if (flock(handle_fd(handle), LOCK_EX | LOCK_NB) == 0)
            return 1;
        last_error = 32;
        return 0;
    case 0x9001c: // FSCTL_UNLOCK_VOLUME
        if (flock(handle_fd(handle), LOCK_UN) == 0)
            return 1;
        last_error = 6;
        return 0;
    case 0x70140: // IOCTL_DISK_UPDATE_PROPERTIES
        if (probe_volume_ready())
            probe_mounted = 1;
        return 1;
    case 0x90020: // FSCTL_DISMOUNT_VOLUME
    case 0x90083: // FSCTL_ALLOW_EXTENDED_DASD_IO
        return 1; // This virtual volume is a regular file and is never mounted.
    case 0x560000: // IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS
        words[0] = 1;
        words[2] = 2;
        wide[2] = probe_volume_ready() ? 0 : 1048576;
        wide[3] = disk.st_size - wide[2];
        length = 32;
        break;
    case 0x2d4800: // IOCTL_STORAGE_CHECK_VERIFY
    case 0x2d0800: // IOCTL_STORAGE_CHECK_VERIFY2
        if (!capacity)
            return 1;
        words[0] = 1;
        length = 4;
        break;
    case 0x2d1080: // IOCTL_STORAGE_GET_DEVICE_NUMBER
        words[0] = 7;
        words[1] = 2;
        words[2] = UINT32_MAX;
        length = 12;
        break;
    case 0x700a0: // IOCTL_DISK_GET_DRIVE_GEOMETRY_EX
    case 0x70000: // IOCTL_DISK_GET_DRIVE_GEOMETRY
        wide[0] = disk.st_size / (512 * 63 * 255);
        words[2] = 11;
        words[3] = 255;
        words[4] = 63;
        words[5] = 512;
        wide[3] = disk.st_size;
        length = code == 0x70000 ? 24 : 32;
        break;
    case 0x70050: // IOCTL_DISK_GET_DRIVE_LAYOUT_EX
        length = disk_layout(handle_fd(handle), disk.st_size, info.bytes);
        if (!length)
            return 0;
        break;
    case 0x7405c: // IOCTL_DISK_GET_LENGTH_INFO
        wide[0] = disk.st_size;
        length = 8;
        break;
    case 0x70024: // IOCTL_DISK_IS_WRITABLE
        if ((fcntl(handle_fd(handle), F_GETFL) & O_ACCMODE) == O_RDONLY) {
            last_error = 19;
            return 0;
        }
        return 1;
    default:
        fprintf(stderr, "Unhandled virtual disk IOCTL %#x in=%u out=%u\n", code, input_size, capacity);
        last_error = 1;
        return 0;
    }
    if (!output || capacity < length) {
        last_error = 122;
        return 0;
    }
    memcpy(output, info.bytes, length);
    if (returned)
        *returned = length;
    return 1;
}

WIN DWORD win_QueryDosDeviceA(const char *name, char *output, DWORD capacity)
{
    static const char boot[] = "\\Device\\MostSandbox\0";
    static const char disk[] = "\\Device\\Harddisk2\\DR2\0";
    static const char volumes[] = "Z:\0PhysicalDrive2\0";
    static const char only_system[] = "Z:\0";
    struct stat info;
    int present = probe_disk_stat(&info);
    const char *value;
    size_t length;
    if (!name) {
        value = present ? volumes : only_system;
        length = present ? sizeof(volumes) : sizeof(only_system);
    } else if (!strcasecmp(name, "Z:")) {
        value = boot;
        length = sizeof(boot);
    } else if (present && (!strcasecmp(name, "PhysicalDrive2") ||
        (probe_volume_ready() && (!strncasecmp(name, probe_volume + 4, sizeof(probe_volume) - 6) ||
                                 (probe_mounted && !strcasecmp(name, "C:")))))) {
        value = disk;
        length = sizeof(disk);
    } else {
        last_error = 2;
        return 0;
    }
    if (!output || capacity < length) {
        last_error = 122;
        return 0;
    }
    memcpy(output, value, length);
    return length;
}

WIN DWORD win_QueryDosDeviceW(const WCHAR *name, WCHAR *output, DWORD capacity)
{
    char ascii[128], buffer[256];
    if (name && !ascii_name(name, ascii, sizeof(ascii))) {
        last_error = 2;
        return 0;
    }
    DWORD size = win_QueryDosDeviceA(name ? ascii : NULL, buffer, sizeof(buffer));
    if (!size)
        return 0;
    if (!output || capacity < size) {
        last_error = 122;
        return 0;
    }
    for (DWORD i = 0; i < size; ++i)
        output[i] = (unsigned char)buffer[i];
    return size;
}

WIN DWORD win_GetLogicalDriveStringsA(DWORD capacity, char *output)
{
    static const char all[] = "C:\\\0Z:\\\0";
    DWORD count = probe_mounted ? 9 : 5;
    if (!output || capacity < count)
        return count;
    memcpy(output, all + 9 - count, count);
    return count - 1;
}

#define VOLUME_SEARCH ((uintptr_t)0x30002)
WIN uintptr_t win_FindFirstVolumeA(char *volume, DWORD capacity)
{
    if (!probe_volume_ready()) {
        last_error = 18;
        return UINTPTR_MAX;
    }
    if (!volume || capacity < sizeof(probe_volume)) {
        last_error = 122;
        return UINTPTR_MAX;
    }
    memcpy(volume, probe_volume, sizeof(probe_volume));
    return VOLUME_SEARCH;
}
WIN uintptr_t win_FindFirstVolumeW(WCHAR *volume, DWORD capacity)
{
    char text[128];
    if (!volume || capacity < sizeof(probe_volume)) {
        last_error = 122;
        return UINTPTR_MAX;
    }
    uintptr_t handle = win_FindFirstVolumeA(text, sizeof(text));
    if (handle != UINTPTR_MAX)
        copy_path(text, volume, capacity);
    return handle;
}
WIN int win_FindNextVolumeA(uintptr_t handle, char *volume, DWORD capacity)
{
    (void)volume; (void)capacity;
    last_error = handle == VOLUME_SEARCH ? 18 : 6;
    return 0;
}
WIN int win_FindNextVolumeW(uintptr_t handle, WCHAR *volume, DWORD capacity)
{
    (void)volume; (void)capacity;
    last_error = handle == VOLUME_SEARCH ? 18 : 6;
    return 0;
}
WIN int win_FindVolumeClose(uintptr_t handle)
{
    if (handle == VOLUME_SEARCH)
        return 1;
    last_error = 6;
    return 0;
}
WIN int win_SetVolumeMountPointA(const char *point, const char *volume)
{
    if (!point || !volume || strcasecmp(point, "C:\\") || !is_probe_volume(volume) || !probe_volume_ready()) {
        last_error = 87;
        return 0;
    }
    probe_mounted = 1;
    return 1;
}
WIN int win_DeleteVolumeMountPointA(const char *point)
{
    if (!point || strcasecmp(point, "C:\\") || !probe_mounted) {
        last_error = 2;
        return 0;
    }
    probe_mounted = 0;
    return 1;
}
WIN int win_GetVolumeNameForVolumeMountPointA(const char *point, char *volume, DWORD capacity)
{
    if (!point || strcasecmp(point, "C:\\") || !probe_mounted) {
        last_error = 2;
        return 0;
    }
    if (!volume || capacity < sizeof(probe_volume)) {
        last_error = 122;
        return 0;
    }
    memcpy(volume, probe_volume, sizeof(probe_volume));
    return 1;
}

WIN int win_GetVolumeInformationByHandleW(uintptr_t handle, WCHAR *label, DWORD label_capacity,
                                             DWORD *serial, DWORD *component, DWORD *flags,
                                             WCHAR *filesystem, DWORD filesystem_capacity)
{
    int fd = handle_fd(handle);
    struct stat info, disk;
    if (fd < 0 || fstat(fd, &info) != 0) {
        last_error = 6;
        return 0;
    }
    char disk_label[12] = {0};
    const char *name = "MostSandbox", *format = "NTFS";
    DWORD volume_serial = info.st_dev;
    if (disk_handle_stat(handle, &disk)) {
        unsigned char sector[512];
        if (!probe_volume_sector(sector)) {
            last_error = 1005;
            return 0;
        }
        int fat32 = read_word(sector + 22) == 0;
        int label_offset = fat32 ? 71 : 43;
        memcpy(disk_label, sector + label_offset, 11);
        for (int i = 10; i >= 0 && disk_label[i] == ' '; --i)
            disk_label[i] = 0;
        name = disk_label;
        format = fat32 ? "FAT32" : "FAT";
        volume_serial = read_dword(sector + (fat32 ? 67 : 39));
    }
    if ((label && copy_path(name, label, label_capacity) >= label_capacity) ||
        (filesystem && copy_path(format, filesystem, filesystem_capacity) >= filesystem_capacity)) {
        last_error = 122;
        return 0;
    }
    if (serial) *serial = volume_serial;
    if (component) *component = 255;
    if (flags) *flags = 3;
    return 1;
}

WIN const char *win_StrStrIA(const char *text, const char *needle)
{
    return text && needle ? strcasestr(text, needle) : NULL;
}

WIN int win_IsDebuggerPresent(void) { return 0; }

typedef struct HeapAllocation {
    struct HeapAllocation *next;
    void *data;
} HeapAllocation;
typedef struct GuestHeap {
    struct GuestHeap *next;
    HeapAllocation *allocations;
} GuestHeap;
static GuestHeap *rt_heaps;
static pthread_mutex_t rt_heap_lock = PTHREAD_MUTEX_INITIALIZER;

WIN void *win_RtlCreateHeap(DWORD flags, void *base, size_t reserve, size_t commit,
                            void *lock, const void *parameters)
{
    (void)flags; (void)reserve; (void)commit; (void)lock; (void)parameters;
    if (base)
        return NULL;
    GuestHeap *heap = calloc(1, sizeof(*heap));
    if (!heap)
        return NULL;
    pthread_mutex_lock(&rt_heap_lock);
    heap->next = rt_heaps;
    rt_heaps = heap;
    pthread_mutex_unlock(&rt_heap_lock);
    return heap;
}

static GuestHeap **find_heap(void *handle)
{
    GuestHeap **link = &rt_heaps;
    while (*link && *link != handle)
        link = &(*link)->next;
    return link;
}

WIN void *win_RtlAllocateHeap(void *handle, DWORD flags, size_t size)
{
    if (handle == (void *)1)
        return win_HeapAlloc(handle, flags, size);
    pthread_mutex_lock(&rt_heap_lock);
    GuestHeap *heap = *find_heap(handle);
    HeapAllocation *allocation = heap ? malloc(sizeof(*allocation)) : NULL;
    void *data = NULL;
    if (allocation) {
        data = flags & 8 ? calloc(1, size ? size : 1) : malloc(size ? size : 1);
        if (data) {
            allocation->data = data;
            allocation->next = heap->allocations;
            heap->allocations = allocation;
        } else {
            free(allocation);
        }
    }
    pthread_mutex_unlock(&rt_heap_lock);
    return data;
}
WIN uint8_t win_RtlFreeHeap(void *handle, DWORD flags, void *data)
{
    if (handle == (void *)1)
        return win_HeapFree(handle, flags, data);
    if (!data)
        return 1;
    pthread_mutex_lock(&rt_heap_lock);
    GuestHeap *heap = *find_heap(handle);
    HeapAllocation *allocation = NULL;
    if (heap) {
        HeapAllocation **link = &heap->allocations;
        while (*link && (*link)->data != data)
            link = &(*link)->next;
        allocation = *link;
        if (allocation)
            *link = allocation->next;
    }
    pthread_mutex_unlock(&rt_heap_lock);
    if (!allocation)
        return 0;
    free(data);
    free(allocation);
    return 1;
}
WIN void *win_RtlDestroyHeap(void *handle)
{
    pthread_mutex_lock(&rt_heap_lock);
    GuestHeap **link = find_heap(handle);
    GuestHeap *heap = *link;
    if (heap)
        *link = heap->next;
    pthread_mutex_unlock(&rt_heap_lock);
    if (!heap)
        return handle;
    while (heap->allocations) {
        HeapAllocation *allocation = heap->allocations;
        heap->allocations = allocation->next;
        free(allocation->data);
        free(allocation);
    }
    free(heap);
    return NULL;
}

WIN int32_t win_NtQuerySystemInformation(DWORD kind, void *output, DWORD size, DWORD *returned)
{
    if (returned)
        *returned = 0;
    if (kind == 64) { // No other Windows processes can hold guest handles in the sandbox.
        if (returned)
            *returned = 16;
        if (!output || size < 16)
            return (int32_t)0xc0000004u;
        memset(output, 0, 16);
        return 0;
    }
    fprintf(stderr, "Unsupported NtQuerySystemInformation class=%u\n", kind);
    return (int32_t)0xc0000003u;
}

int probe_is_virtual_disk(void)
{
    struct stat disk;
    return native_usb_fd < 0 && probe_disk_stat(&disk);
}

WIN int win_GetOverlappedResultEx(uintptr_t handle, Overlapped *request, DWORD *completed,
                                  DWORD timeout, int alertable)
{
    (void)alertable;
    if (handle_fd(handle) < 0 || !request || !completed) {
        last_error = 87;
        return 0;
    }
    *completed = 0;
    if (request->internal == 0x103) {
        if (!request->event || !timeout) {
            last_error = 996;
            return 0;
        }
        DWORD state = win_WaitForSingleObject(request->event & ~(uintptr_t)1, timeout);
        if (state || request->internal == 0x103) {
            last_error = state == 258 ? 258 : 996;
            return 0;
        }
    }
    if (request->internal) {
        last_error = 1117;
        return 0;
    }
    *completed = request->bytes;
    return 1;
}
WIN int win_GetOverlappedResult(uintptr_t handle, Overlapped *request, DWORD *completed, int wait)
{
    return win_GetOverlappedResultEx(handle, request, completed, wait ? UINT32_MAX : 0, 0);
}
WIN int win_CancelIoEx(uintptr_t handle, Overlapped *request)
{
    (void)request;
    last_error = handle_fd(handle) < 0 ? 6 : 1168;
    return 0;
}

typedef struct { uintptr_t status, information; } IoStatus;
WIN int32_t win_NtQueryVolumeInformationFile(uintptr_t handle, IoStatus *status, void *data,
                                               DWORD capacity, DWORD kind)
{
    if (!status)
        return (int32_t)0xc000000du;
    status->information = 0;
    int32_t result;
    struct stat disk;
    if (handle_fd(handle) < 0)
        result = (int32_t)0xc0000008u;
    else if (kind != 4)
        result = (int32_t)0xc0000003u;
    else if (!data || capacity < 8)
        result = (int32_t)0xc0000023u;
    else {
        DWORD flags = disk_handle_stat(handle, &disk) ? 1 : 0;
        if (flags && probe_volume_ready()) flags |= 0x20;
        DWORD info[] = {7, flags};
        memcpy(data, info, sizeof(info));
        status->information = sizeof(info);
        result = 0;
    }
    status->status = (uint32_t)result;
    return result;
}

WIN int win_GetVolumeInformationW(const WCHAR *root, WCHAR *label, DWORD label_capacity,
                                    DWORD *serial, DWORD *component, DWORD *flags,
                                    WCHAR *filesystem, DWORD filesystem_capacity)
{
    static const WCHAR system_root[] = {'Z', ':', '\\', 0};
    static const WCHAR scratch[] = {'Z', ':', '\\', 's','c','r','a','t','c','h',0};
    if (!root || env_equal(root, system_root))
        root = scratch;
    uintptr_t handle = win_CreateFileW(root, 0x80000000u, 3, NULL, 3, 0, 0);
    if (handle == UINTPTR_MAX)
        return 0;
    int result = win_GetVolumeInformationByHandleW(handle, label, label_capacity, serial, component, flags,
                                                   filesystem, filesystem_capacity);
    DWORD error = last_error;
    win_CloseHandle(handle);
    last_error = error;
    return result;
}
WIN int win_GetVolumeInformationA(const char *root, char *label, DWORD label_capacity,
                                    DWORD *serial, DWORD *component, DWORD *flags,
                                    char *filesystem, DWORD filesystem_capacity)
{
    WCHAR path[512], name[260], format[260];
    if (root && !win_MultiByteToWideChar(0, 0, root, -1, path, 512))
        return 0;
    if (!win_GetVolumeInformationW(root ? path : NULL, label ? name : NULL, 260, serial,
                                   component, flags, filesystem ? format : NULL, 260))
        return 0;
    if ((label && !win_WideCharToMultiByte(0, 0, name, -1, label, label_capacity, NULL, NULL)) ||
        (filesystem && !win_WideCharToMultiByte(0, 0, format, -1, filesystem, filesystem_capacity, NULL, NULL)))
        return 0;
    return 1;
}

WIN int win_PathFileExistsA(const char *name)
{
    WCHAR wide[512];
    return win_MultiByteToWideChar(0, 0, name, -1, wide, 512) && win_PathFileExistsW(wide);
}

WIN int win_PathGetDriveNumberW(const WCHAR *path)
{
    if (!path || path[1] != ':')
        return -1;
    unsigned letter = ascii_fold(path[0]);
    return letter >= 'a' && letter <= 'z' ? (int)(letter - 'a') : -1;
}
