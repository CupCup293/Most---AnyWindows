#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <iconv.h>
#include <limits.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include "entry.h"

#define WIN __attribute__((ms_abi))
typedef uint16_t WCHAR;
typedef uint32_t DWORD;

static uint64_t clock_100ns(clockid_t kind)
{
    struct timespec time;
    if (clock_gettime(kind, &time) != 0)
        abort();
    return (uint64_t)time.tv_sec * 10000000 + time.tv_nsec / 100;
}

WIN void win_GetSystemTimeAsFileTime(uint64_t *filetime)
{
    *filetime = 116444736000000000ull + clock_100ns(CLOCK_REALTIME);
}

WIN int win_QueryPerformanceCounter(uint64_t *counter)
{
    *counter = clock_100ns(CLOCK_MONOTONIC);
    return 1;
}

WIN int win_QueryPerformanceFrequency(uint64_t *frequency)
{
    *frequency = 10000000;
    return 1;
}

WIN DWORD win_GetCurrentProcessId(void) { return getpid(); }
WIN DWORD win_GetCurrentThreadId(void) { return syscall(SYS_gettid); }
WIN uintptr_t win_GetCurrentProcess(void) { return (uintptr_t)-1; }
WIN uintptr_t win_GetCurrentThread(void) { return (uintptr_t)-2; }
#ifdef MOST_FILEINFO
static int fileinfo_failed;
#endif
#ifdef MOST_REPTXT
static int reptxt_failed;
#endif

WIN void win_ExitProcess(DWORD status)
{
#ifdef MOST_FILEINFO
    fflush(NULL);
    if (!status && fileinfo_failed)
        status = 1;
#endif
    _Exit(status);
}
WIN int win_TerminateProcess(uintptr_t process, DWORD status)
{
    if (process != (uintptr_t)-1)
        return 0;
    _Exit(status);
}

static void *exception_filter;
WIN void *win_SetUnhandledExceptionFilter(void *handler)
{
    void *old = exception_filter;
    exception_filter = handler;
    return old;
}

WIN int win_IsProcessorFeaturePresent(DWORD feature)
{
    switch (feature) {
    case 6:  // SSE
    case 10: // SSE2
    case 13: // SSE3
        return 1;
    default:
        return 0;
    }
}

WIN void *win_GetProcessHeap(void) { return (void *)1; }
WIN void *win_HeapAlloc(void *heap, DWORD flags, size_t size)
{
    if (heap != (void *)1)
        return NULL;
    return flags & 8 ? calloc(1, size ? size : 1) : malloc(size ? size : 1);
}
WIN int win_HeapFree(void *heap, DWORD flags, void *ptr)
{
    (void)flags;
    if (heap != (void *)1)
        return 0;
    free(ptr);
    return 1;
}

extern DWORD *guest_last_error(void);
#define last_error (*guest_last_error())
WIN void win_SetLastError(DWORD value) { last_error = value; }
WIN DWORD win_GetLastError(void) { return last_error; }


typedef struct {
    pthread_mutex_t *mutex;
    unsigned char reserved[32];
} CriticalSection;

WIN int win_InitializeCriticalSectionEx(CriticalSection *section, DWORD spin, DWORD flags)
{
    (void)spin;
    (void)flags;
    pthread_mutex_t *mutex = malloc(sizeof(*mutex));
    if (!mutex)
        return 0;
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr)) {
        free(mutex);
        return 0;
    }
    int error = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    if (!error)
        error = pthread_mutex_init(mutex, &attr);
    pthread_mutexattr_destroy(&attr);
    if (error) {
        free(mutex);
        return 0;
    }
    memset(section, 0, sizeof(*section));
    section->mutex = mutex;
    return 1;
}
WIN void win_InitializeCriticalSection(CriticalSection *section)
{
    if (!win_InitializeCriticalSectionEx(section, 0, 0))
        abort();
}
WIN void win_EnterCriticalSection(CriticalSection *section) { pthread_mutex_lock(section->mutex); }
WIN void win_LeaveCriticalSection(CriticalSection *section) { pthread_mutex_unlock(section->mutex); }
WIN void win_DeleteCriticalSection(CriticalSection *section)
{
    pthread_mutex_destroy(section->mutex);
    free(section->mutex);
}

typedef WIN void (*FlsCallback)(void *);
static struct { pthread_key_t key; FlsCallback callback; int used; } fls[128];

WIN DWORD win_FlsAlloc(FlsCallback callback)
{
    for (DWORD i = 0; i < 128; ++i) {
        if (!fls[i].used) {
            if (pthread_key_create(&fls[i].key, NULL))
                break;
            fls[i].callback = callback;
            fls[i].used = 1;
            return i;
        }
    }
    last_error = 8;
    return UINT32_MAX;
}

WIN void *win_FlsGetValue(DWORD index)
{
    if (index < 128 && fls[index].used) {
        last_error = 0;
        return pthread_getspecific(fls[index].key);
    }
    last_error = 87;
    return NULL;
}

WIN int win_FlsSetValue(DWORD index, void *value)
{
    if (index < 128 && fls[index].used && !pthread_setspecific(fls[index].key, value))
        return 1;
    last_error = 87;
    return 0;
}

WIN int win_FlsFree(DWORD index)
{
    if (index >= 128 || !fls[index].used) {
        last_error = 87;
        return 0;
    }
    void *value = pthread_getspecific(fls[index].key);
    if (value && fls[index].callback)
        fls[index].callback(value);
    pthread_key_delete(fls[index].key);
    fls[index].used = 0;
    return 1;
}

WIN void win_GetStartupInfoW(void *startup)
{
    memset(startup, 0, 104);
    *(DWORD *)startup = 104;
}

#if defined(MOST_RAWCOPY)
#define GUEST_NAME "rawcopy.exe"
#define GUEST_WIDE_NAME u"rawcopy.exe"
#elif defined(MOST_EXELIST)
#define GUEST_NAME "exelist.exe"
#define GUEST_WIDE_NAME u"exelist.exe"
#elif defined(MOST_GETHOST)
#define GUEST_NAME "gethost.exe"
#define GUEST_WIDE_NAME u"gethost.exe"
#elif defined(MOST_CUT)
#define GUEST_NAME "cut.exe"
#define GUEST_WIDE_NAME u"cut.exe"
#elif defined(MOST_ELTORITO)
#define GUEST_NAME "geteltorito.exe"
#define GUEST_WIDE_NAME u"geteltorito.exe"
#elif defined(MOST_REPTXT)
#define GUEST_NAME "reptxt32.exe"
#define GUEST_WIDE_NAME u"reptxt32.exe"
#elif defined(MOST_FILEINFO)
#define GUEST_NAME "fileinfo.exe"
#define GUEST_WIDE_NAME u"fileinfo.exe"
#elif defined(MOST_FSPLIT)
#define GUEST_NAME "fsplit32.exe"
#define GUEST_WIDE_NAME u"fsplit32.exe"
#elif defined(MOST_CHSIZE)
#define GUEST_NAME "chsize32.exe"
#define GUEST_WIDE_NAME u"chsize32.exe"
#elif defined(MOST_W32VER)
#define GUEST_NAME "w32ver.exe"
#define GUEST_WIDE_NAME u"w32ver.exe"
#else
#define GUEST_NAME "rufus.exe"
#define GUEST_WIDE_NAME u"rufus.exe"
#endif
#define GUEST_DOS_PATH "Z:\\" GUEST_NAME
static char command[] = GUEST_NAME;
static WCHAR wide_command[] = GUEST_WIDE_NAME;
WIN char *win_GetCommandLineA(void) { return command; }
WIN WCHAR *win_GetCommandLineW(void) { return wide_command; }
WIN DWORD win_GetACP(void) { return 1252; }
WIN DWORD win_GetOEMCP(void) { return 437; }
WIN int win_IsValidCodePage(DWORD page)
{
    return page == 1252 || page == 437 || page == 65001;
}

typedef struct EnvironmentEntry {
    struct EnvironmentEntry *next;
    WCHAR *name, *value;
} EnvironmentEntry;
static EnvironmentEntry *guest_environment;
static pthread_mutex_t environment_lock = PTHREAD_MUTEX_INITIALIZER;

static size_t wide_length(const WCHAR *text)
{
    size_t size = 0;
    while (text[size])
        ++size;
    return size;
}
static WCHAR *wide_duplicate(const WCHAR *text)
{
    size_t size = (wide_length(text) + 1) * sizeof(WCHAR);
    WCHAR *copy = malloc(size);
    if (copy) memcpy(copy, text, size);
    return copy;
}
static unsigned ascii_fold(WCHAR ch)
{
    return ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch;
}
static int env_equal(const WCHAR *left, const WCHAR *right)
{
    while (*left && ascii_fold(*left) == ascii_fold(*right)) {
        ++left;
        ++right;
    }
    return !*left && !*right;
}
static EnvironmentEntry **env_find(const WCHAR *name)
{
    EnvironmentEntry **entry = &guest_environment;
    while (*entry && !env_equal((*entry)->name, name))
        entry = &(*entry)->next;
    return entry;
}
WIN WCHAR *win_GetEnvironmentStringsW(void)
{
    pthread_mutex_lock(&environment_lock);
    size_t chars = 2;
    for (const EnvironmentEntry *entry = guest_environment; entry; entry = entry->next)
        chars += wide_length(entry->name) + wide_length(entry->value) + 2;
    WCHAR *block = calloc(chars, sizeof(WCHAR));
    if (block) {
        WCHAR *out = block;
        for (const EnvironmentEntry *entry = guest_environment; entry; entry = entry->next) {
            size_t name = wide_length(entry->name), value = wide_length(entry->value);
            memcpy(out, entry->name, name * sizeof(WCHAR));
            out += name;
            *out++ = '=';
            memcpy(out, entry->value, value * sizeof(WCHAR));
            out += value + 1;
        }
    }
    pthread_mutex_unlock(&environment_lock);
    return block;
}
WIN int win_FreeEnvironmentStringsW(WCHAR *value) { free(value); return 1; }
WIN DWORD win_GetEnvironmentVariableW(const WCHAR *name, WCHAR *value, DWORD size)
{
    if (!name || !*name) {
        last_error = 87;
        return 0;
    }
    pthread_mutex_lock(&environment_lock);
    const EnvironmentEntry *entry = *env_find(name);
    DWORD result = 0;
    if (!entry) {
        last_error = 203;
    } else {
        size_t length = wide_length(entry->value);
        if (!value || size <= length)
            result = length + 1;
        else {
            memcpy(value, entry->value, (length + 1) * sizeof(WCHAR));
            result = length;
        }
    }
    pthread_mutex_unlock(&environment_lock);
    return result;
}
WIN int win_SetEnvironmentVariableW(const WCHAR *name, const WCHAR *value)
{
    if (!name || !*name) {
        last_error = 87;
        return 0;
    }
    pthread_mutex_lock(&environment_lock);
    EnvironmentEntry **link = env_find(name);
    EnvironmentEntry *old = *link;
    EnvironmentEntry *entry = NULL;
    if (value) {
        entry = calloc(1, sizeof(*entry));
        if (entry) {
            entry->name = wide_duplicate(name);
            entry->value = wide_duplicate(value);
        }
        if (!entry || !entry->name || !entry->value) {
            if (entry) {
                free(entry->name);
                free(entry->value);
                free(entry);
            }
            pthread_mutex_unlock(&environment_lock);
            last_error = 8;
            return 0;
        }
        entry->next = old ? old->next : NULL;
    }
    *link = entry ? entry : old ? old->next : NULL;
    if (old) {
        free(old->name);
        free(old->value);
        free(old);
    }
    pthread_mutex_unlock(&environment_lock);
    if (!entry && !old) {
        last_error = 203;
        return 0;
    }
    return 1;
}

WIN uintptr_t win_GetStdHandle(DWORD which)
{
    if (which >= (DWORD)-12 && which <= (DWORD)-10)
        return (DWORD)-which - 9;
    last_error = 87;
    return (uintptr_t)-1;
}
static int handle_fd(uintptr_t handle);
WIN DWORD win_GetFileType(uintptr_t handle)
{
    struct stat info;
    int fd = handle_fd(handle);
    if (fd < 0 || fstat(fd, &info) != 0) {
        last_error = 6;
        return 0;
    }
    if (S_ISCHR(info.st_mode))
        return 2;
    if (S_ISFIFO(info.st_mode) || S_ISSOCK(info.st_mode))
        return 3;
    return 1;
}

static struct { uintptr_t address; size_t size; DWORD protection; } image_sections[] = {
#include "pages.h"
};
static DWORD guest_protection[0x2000];

static int linux_protection(DWORD protect)
{
    switch (protect) {
    case 1: return PROT_NONE;
    case 2: return PROT_READ;
    case 4: return PROT_READ | PROT_WRITE;
    case 0x10: return PROT_EXEC;
    case 0x20: return PROT_READ | PROT_EXEC;
    default: return -1;
    }
}

WIN int win_VirtualProtect(void *ptr, size_t length, DWORD protect, DWORD *previous)
{
    uintptr_t address = (uintptr_t)ptr;
    uintptr_t page = address & ~(uintptr_t)4095;
    if (page < GUEST_BASE || address - GUEST_BASE >= sizeof(guest_protection) * 1024 ||
        !length || length > sizeof(guest_protection) * 1024 - (address - GUEST_BASE) || !previous) {
        last_error = 87;
        return 0;
    }
    size_t first = (page - GUEST_BASE) / 4096;
    size_t pages = (address - page + length + 4095) / 4096;
    if (!guest_protection[0]) {
        for (size_t i = 0; i < sizeof(image_sections) / sizeof(image_sections[0]); ++i) {
            size_t start = (image_sections[i].address - GUEST_BASE) / 4096;
            size_t count = (image_sections[i].size + 4095) / 4096;
            for (size_t j = 0; j < count; ++j)
                guest_protection[start + j] = image_sections[i].protection;
        }
    }
    int flags = linux_protection(protect);
    if (flags < 0) {
        last_error = 87;
        return 0;
    }
    for (size_t i = first; i < first + pages; ++i) {
        if (!guest_protection[i]) {
            last_error = 487;
            return 0;
        }
    }
    if (mprotect((void *)page, pages * 4096, flags)) {
        last_error = 5;
        return 0;
    }
    *previous = guest_protection[first];
    for (size_t i = first; i < first + pages; ++i)
        guest_protection[i] = protect;
    return 1;
}

static const char *const modules[] = {
    "msvcrt.dll",
#include "modules.h"
};

static int same_module(const char *left, const char *right)
{
    size_t n = strlen(left), m = strlen(right);
    if (n >= 4 && !strcasecmp(left + n - 4, ".dll"))
        n -= 4;
    if (m >= 4 && !strcasecmp(right + m - 4, ".dll"))
        m -= 4;
    return n == m && !strncasecmp(left, right, n);
}

static void *find_module(const char *name)
{
    if (!name)
        return (void *)GUEST_BASE;
    if (same_module(name, "kernelbase") ||
        !strcasecmp(name, "api-ms-win-core-fibers-l1-1-2"))
        name = "kernel32";
    for (size_t i = 0; i < sizeof(modules) / sizeof(modules[0]); ++i)
        if (same_module(modules[i], name))
            return (void *)&modules[i];
    last_error = 126;
    fprintf(stderr, "Unknown module %s\n", name);
    return NULL;
}

static int ascii_name(const WCHAR *wide, char *buffer, size_t size)
{
    size_t i = 0;
    while (i + 1 < size && wide[i] && wide[i] <= 127) {
        buffer[i] = wide[i];
        ++i;
    }
    buffer[i] = 0;
    return !wide[i];
}

WIN void *win_GetModuleHandleA(const char *name) { return find_module(name); }
WIN void *win_GetModuleHandleW(const WCHAR *name)
{
    char buffer[512];
    if (!name)
        return (void *)GUEST_BASE;
    if (ascii_name(name, buffer, sizeof(buffer)))
        return find_module(buffer);
    last_error = 126;
    return NULL;
}
WIN int win_GetModuleHandleExW(DWORD flags, const WCHAR *name, void **output)
{
    if (!output || (flags & ~7u) || ((flags & 1) && (flags & 2))) {
        last_error = 87;
        return 0;
    }
    *output = NULL;
    if (flags & 4) {
        uintptr_t address = (uintptr_t)name;
        if (address >= GUEST_BASE && address - GUEST_BASE < GUEST_SIZE)
            *output = (void *)GUEST_BASE;
        else
            last_error = 126;
    } else {
        *output = win_GetModuleHandleW(name);
    }
    return *output != NULL;
}

WIN void *win_LoadLibraryW(const WCHAR *name) { return win_GetModuleHandleW(name); }
WIN void *win_LoadLibraryA(const char *name) { return find_module(name); }
WIN void *win_LoadLibraryExW(const WCHAR *name, void *file, DWORD flags)
{
    (void)file;
    (void)flags;
    return win_GetModuleHandleW(name);
}
WIN int win_FreeLibrary(void *handle)
{
    for (size_t i = 0; i < sizeof(modules) / sizeof(modules[0]); ++i)
        if (handle == &modules[i])
            return 1;
    last_error = 6;
    return 0;
}

static const char *codepage(DWORD page)
{
    switch (page) {
#ifdef MOST_FILEINFO
    case 0: case 1: case 3: return "UTF-8";
    case 1252: return "CP1252";
    case 437: return "CP437";
#else
    case 0: case 3: case 1252: return "CP1252";
    case 1: case 437: return "CP437";
#endif
    case 65001: return "UTF-8";
    default: return NULL;
    }
}
WIN int win_GetCPInfo(DWORD page, void *info)
{
    if (!codepage(page) || !info) {
        last_error = 87;
        return 0;
    }
    memset(info, 0, 20);
    *(DWORD *)info = page == 65001 ? 4 : 1;
    ((char *)info)[4] = '?';
    return 1;
}

static int transcode(const char *from, const char *to, const void *source, size_t length,
                     void *output, int capacity, size_t unit)
{
    if (!from || !to || !source || capacity < 0 || (capacity && !output)) {
        last_error = 87;
        return 0;
    }
    iconv_t converter = iconv_open(to, from);
    if (converter == (iconv_t)-1) {
        last_error = 87;
        return 0;
    }
    size_t remaining = length, written = 0;
    char *input = (char *)source;
    int result = 0;
    do {
        char buffer[512];
        char *target = capacity ? (char *)output : buffer;
        size_t space = capacity ? (size_t)capacity * unit : sizeof(buffer);
        size_t initial = space;
        size_t status = iconv(converter, &input, &remaining, &target, &space);
        written += initial - space;
        if (status != (size_t)-1) {
            result = written / unit <= INT_MAX ? written / unit : 0;
            break;
        }
        if (!capacity && errno == E2BIG && initial != space)
            continue;
        last_error = errno == E2BIG ? 122 : 1113;
        break;
    } while (remaining);
    iconv_close(converter);
    return result;
}

WIN int win_MultiByteToWideChar(DWORD page, DWORD flags, const char *source, int length,
                                WCHAR *output, int capacity)
{
    (void)flags;
    if (!source || length < -1 || length == 0) {
        last_error = 87;
        return 0;
    }
    size_t bytes = length == -1 ? strlen(source) + 1 : (size_t)length;
    return transcode(codepage(page), "UTF-16LE", source, bytes, output, capacity, 2);
}

WIN int win_WideCharToMultiByte(DWORD page, DWORD flags, const WCHAR *source, int length,
                                char *output, int capacity, const char *fallback, int *used)
{
    (void)flags;
    (void)fallback;
    if (used)
        *used = 0;
    if (!source || length < -1 || length == 0) {
        last_error = 87;
        return 0;
    }
    size_t chars = 0;
    if (length == -1) {
        while (source[chars++]) {}
    } else {
        chars = length;
    }
    return transcode("UTF-16LE", codepage(page), source, chars * 2, output, capacity, 1);
}

WIN DWORD win_GetModuleFileNameW(void *module, WCHAR *buffer, DWORD capacity)
{
    static const char path[] = GUEST_DOS_PATH;
    if ((module && module != (void *)GUEST_BASE) || !buffer || !capacity) {
        last_error = 87;
        return 0;
    }
    DWORD length = sizeof(path) - 1;
    if (length >= capacity) {
        for (DWORD i = 0; i + 1 < capacity; ++i)
            buffer[i] = path[i];
        buffer[capacity - 1] = 0;
        last_error = 122;
        return capacity;
    }
    for (DWORD i = 0; i <= length; ++i)
        buffer[i] = path[i];
    return length;
}

WIN DWORD win_K32GetModuleFileNameExW(void *process, void *module, WCHAR *buffer, DWORD capacity)
{
    if (process != (void *)-1) {
        last_error = 6;
        return 0;
    }
    return win_GetModuleFileNameW(module, buffer, capacity);
}

typedef struct {
    DWORD architecture, page_size;
    uintptr_t min_addr, max_addr, mask;
    DWORD processor_count, processor_type, granularity;
    uint16_t level, revision;
} SystemInfo;

WIN void win_GetSystemInfo(SystemInfo *info)
{
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    if (count < 1)
        count = 1;
    *info = (SystemInfo){9, 4096, 0x10000, 0x7ffffffeffffull,
        count >= 64 ? UINT64_MAX : ((1ull << count) - 1), (DWORD)count, 8664, 65536, 6, 0};
}
WIN void win_GetNativeSystemInfo(SystemInfo *info) { win_GetSystemInfo(info); }

WIN void win_InitializeSListHead(void *head) { memset(head, 0, 16); }
WIN int win_AreFileApisANSI(void) { return 1; }

#include "resource.inc"
#include "host.inc"
#if defined(MOST_RAWCOPY) || defined(MOST_CHSIZE) || defined(MOST_FSPLIT) || defined(MOST_FILEINFO) || defined(MOST_REPTXT) || defined(MOST_ELTORITO) || defined(MOST_CUT) || defined(MOST_GETHOST) || defined(MOST_EXELIST)
#include "console.inc"
#endif
#if defined(MOST_REPTXT) || defined(MOST_EXELIST)
#include "reptxt.inc"
#endif
#ifdef MOST_CUT
#include "cut.inc"
#endif
#ifdef MOST_GETHOST
#include "winsock.inc"
#endif
#ifdef MOST_EXELIST
#include "exelist.inc"
#endif
#include "exports.h"
#if defined(MOST_W32VER) || defined(MOST_RAWCOPY) || defined(MOST_FSPLIT) || defined(MOST_REPTXT) || defined(MOST_CUT) || defined(MOST_EXELIST)
WIN void *win_memset(void *destination, int value, size_t length)
{
    return memset(destination, value, length);
}
#endif
extern WIN int win_MessageBoxW(void *, const WCHAR *, const WCHAR *, DWORD);
WIN void *win_GetProcAddress(void *handle, const char *name);
static const struct { const char *dll; const char *name; void *entry; } imports[] = {
#define WIN_IMPORT(symbol, dll, name) {dll, name, symbol},
#define WIN_DATA_IMPORT(symbol, dll, name) {dll, name, &(symbol)},
#include "imports.h"
#undef WIN_DATA_IMPORT
#undef WIN_IMPORT
};
WIN void *win_GetProcAddress(void *handle, const char *name)
{
    const char *dll = NULL;
    for (size_t i = 0; i < sizeof(modules) / sizeof(modules[0]); ++i)
        if (handle == &modules[i])
            dll = modules[i];
    if (dll && (uintptr_t)name > UINT16_MAX) {
#ifdef MOST_W32VER
        if (same_module(dll, "ntdll") && !strcmp(name, "RtlGetVersion"))
            return win_RtlGetVersion;
#endif
        if (same_module(dll, "user32") && !strcmp(name, "MessageBoxW"))
            return win_MessageBoxW;
        if (same_module(dll, "msvcrt") && !strcmp(name, "__wgetmainargs"))
            return win___wgetmainargs;
        if (same_module(dll, "kernel32") && !strcmp(name, "IsWow64Process2"))
            return win_IsWow64Process2;
        if (same_module(dll, "kernel32") && !strcmp(name, "AreFileApisANSI"))
            return win_AreFileApisANSI;
        for (size_t i = 0; i < sizeof(imports) / sizeof(imports[0]); ++i)
            if (same_module(dll, imports[i].dll) && !strcmp(name, imports[i].name))
                return imports[i].entry;
        fprintf(stderr, "Unknown procedure %s!%s\n", dll, name);
    }
    last_error = 127;
    return NULL;
}
