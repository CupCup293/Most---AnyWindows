#include <glob.h>

typedef struct {
    DWORD attributes;
    DWORD creation[2], access[2], write[2], size_high, size_low, reserved[2];
    char name[260], alternate[14];
} FindDataA;

typedef struct {
    glob_t matches;
    size_t next;
} Search;

WIN int win_FindNextFileA(Search *search, FindDataA *data)
{
    while (search->next < search->matches.gl_pathc) {
        const char *path = search->matches.gl_pathv[search->next++];
        const char *name = strrchr(path, '/');
        name = name ? name + 1 : path;
        struct stat info;
        if (strlen(name) >= sizeof(data->name) || lstat(path, &info))
            continue;
#ifdef MOST_EXELIST
        if (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode))
#else
        if (strlen(path) > 240 || !S_ISREG(info.st_mode))
#endif
            continue;
        memset(data, 0, sizeof(*data));
        // EXEList skips the two navigation entries itself. They must pass
        // its hidden-file filter so it can finish searching leaf directories.
        int hidden = *name == '.' && strcmp(name, ".") && strcmp(name, "..");
        data->attributes = (hidden ? 2 : 0) | (!(info.st_mode & 0222) ? 1 : 0);
        if (S_ISDIR(info.st_mode))
            data->attributes |= 0x10;
        if (!data->attributes)
            data->attributes = 0x80;
        data->size_high = (uint64_t)info.st_size >> 32;
        data->size_low = info.st_size;
        memcpy(data->name, name, strlen(name) + 1);
        return 1;
    }
    last_error = 18;
    return 0;
}

WIN int win_FindClose(Search *search)
{
    globfree(&search->matches);
    free(search);
    return 1;
}

#ifdef MOST_EXELIST
static int exelist_path(const char *input, char *output, size_t capacity);
#endif
WIN Search *win_FindFirstFileA(const char *pattern, FindDataA *data)
{
    Search *search = calloc(1, sizeof(*search));
    if (!search) {
        last_error = 8;
        return (Search *)-1;
    }
#ifdef MOST_EXELIST
    char linux_path[1040];
    if (!exelist_path(pattern, linux_path, sizeof(linux_path))) {
        free(search);
        last_error = 123;
        return (Search *)-1;
    }
    char *basename = strrchr(linux_path, '/');
    basename = basename ? basename + 1 : linux_path;
    if (!strcmp(basename, "*.*"))
        basename[1] = 0;
    const char *path = linux_path;
#else
    const char *path = !strncmp(pattern, "Z:/", 3) ? pattern + 2 : pattern;
#endif
    if (!glob(path, GLOB_PERIOD | GLOB_NOESCAPE, NULL, &search->matches) &&
        win_FindNextFileA(search, data))
        return search;
    win_FindClose(search);
#ifdef MOST_REPTXT
    fprintf(stderr, "Reptxt found no regular files matching %s.\n", path);
    reptxt_failed = 1;
#endif
    last_error = 2;
    return (Search *)-1;
}

#ifdef MOST_REPTXT
WIN int win_SetFileAttributesA(const char *path, DWORD attributes)
{
    (void)path;
    // Reptxt clears and restores Windows attributes around a write. Unix
    // dotfiles keep their names and permissions throughout.
    return (attributes & ~(0x80u | 2u)) == 0;
}

WIN uintptr_t win_CreateFileMappingA(uintptr_t handle, void *security, DWORD protect,
                                    DWORD high, DWORD low, const char *name)
{
    (void)security;
    int fd = handle_fd(handle);
    if (fd < 0 || protect != 0x18000004u || high || low || name) {
        last_error = 87;
        return 0;
    }
    int copy = fcntl(fd, F_DUPFD_CLOEXEC, 3);
    if (copy < 0) {
        last_error = 8;
        return 0;
    }
    return fd_handle(copy);
}

static void *reptxt_view;
static size_t reptxt_view_size;

WIN void *win_MapViewOfFile(uintptr_t handle, DWORD access, DWORD high, DWORD low, size_t size)
{
    int fd = handle_fd(handle);
    struct stat info;
    if (reptxt_view || fd < 0 || access != 0xf001f || high || low || size ||
        fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_size <= 0 ||
        info.st_size > INT32_MAX) {
        last_error = 87;
        return NULL;
    }
    void *view = mmap(NULL, info.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (view == MAP_FAILED) {
        last_error = 8;
        return NULL;
    }
    reptxt_view = view;
    reptxt_view_size = info.st_size;
    return view;
}

WIN int win_UnmapViewOfFile(void *view)
{
    if (!view || view != reptxt_view) {
        last_error = 87;
        return 0;
    }
    int failed = msync(view, reptxt_view_size, MS_SYNC) != 0;
    if (munmap(view, reptxt_view_size) != 0) {
        last_error = 5;
        return 0;
    }
    reptxt_view = NULL;
    reptxt_view_size = 0;
    if (failed) {
        perror("Reptxt could not sync its changes");
        reptxt_failed = 1;
        last_error = 5;
    }
    return !failed;
}

WIN int win_CompareStringA(DWORD locale, DWORD flags, const char *a, int alen,
                          const char *b, int blen)
{
    (void)locale;
    if (alen < 0 || blen < 0)
        return 0;
    for (int i = 0; i < alen && i < blen; ++i) {
        unsigned x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (flags & 1) {
            x = ascii_fold(x);
            y = ascii_fold(y);
        }
        if (x != y)
            return x < y ? 1 : 3;
    }
    return alen == blen ? 2 : alen < blen ? 1 : 3;
}

WIN int win_GetConsoleScreenBufferInfo(uintptr_t handle, void *info)
{
    (void)handle; (void)info;
    return 0; // Skip Win32 cursor-based progress rendering on a Linux console.
}

WIN char *win_fgets(char *buffer, int size, const void *stream)
{
    FILE *file = raw_stream(stream);
    return file ? fgets(buffer, size, file) : NULL;
}
WIN char *win_strrchr(const char *text, int character)
{
    return strrchr(text, character == '\\' ? '/' : character);
}
WIN char *win_strncpy(char *dst, const char *src, size_t n) { return strncpy(dst, src, n); }
WIN void *win_memcpy(void *dst, const void *src, size_t n) { return memcpy(dst, src, n); }

#else
WIN int win_GetConsoleScreenBufferInfo(uintptr_t handle, void *info)
{
    (void)handle; (void)info;
    return 0;
}
WIN char *win_strncpy(char *dst, const char *src, size_t n) { return strncpy(dst, src, n); }
#endif
