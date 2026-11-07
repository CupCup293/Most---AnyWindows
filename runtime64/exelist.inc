static int exelist_path(const char *input, char *output, size_t capacity)
{
    if (!input)
        return 0;
    if ((input[0] == 'Z' || input[0] == 'z') && input[1] == ':')
        input += 2;
    size_t length = strlen(input);
    if (!length || length >= capacity || strchr(input, ':'))
        return 0;
    for (size_t i = 0; i <= length; ++i)
        output[i] = input[i] == '\\' ? '/' : input[i];
    return 1;
}

static DWORD exelist_windows_path(const char *path, DWORD capacity, char *output, char **part)
{
    size_t length = strlen(path) + 2;
    if (length >= capacity)
        return length + 1;
    output[0] = 'Z';
    output[1] = ':';
    if (part)
        *part = output + 2;
    for (size_t i = 2; i <= length; ++i) {
        output[i] = path[i - 2] == '/' ? '\\' : path[i - 2];
        if (part && output[i] == '\\')
            *part = output + i + 1;
    }
    return length;
}

WIN DWORD win_GetCurrentDirectoryA(DWORD capacity, char *output)
{
    char path[1040];
    return getcwd(path, sizeof(path)) ? exelist_windows_path(path, capacity, output, NULL) : 0;
}

WIN int win_SetCurrentDirectoryA(const char *directory)
{
    char path[1040];
    if (!exelist_path(directory, path, sizeof(path)) || chdir(path)) {
        last_error = 3;
        return 0;
    }
    return 1;
}

WIN DWORD win_GetFullPathNameA(const char *name, DWORD capacity, char *output, char **part)
{
    char path[1040], cwd[1040], full[2080];
    if (!exelist_path(name, path, sizeof(path))) {
        last_error = 123;
        return 0;
    }
    if (*path == '/')
        return exelist_windows_path(path, capacity, output, part);
    if (!getcwd(cwd, sizeof(cwd)))
        return 0;
    snprintf(full, sizeof(full), "%s/%s", cwd, path);
    return exelist_windows_path(full, capacity, output, part);
}

WIN DWORD win_SearchPathA(const char *directories, const char *name, const char *extension,
                         DWORD capacity, char *output, char **part)
{
    (void)directories;
    char path[1040];
    struct stat info;
    if (!exelist_path(name, path, sizeof(path))) {
        last_error = 123;
        return 0;
    }
    if (!lstat(path, &info) && S_ISREG(info.st_mode))
        return win_GetFullPathNameA(path, capacity, output, part);
    const char *basename = strrchr(path, '/');
    basename = basename ? basename + 1 : path;
    size_t length = strlen(path);
    if (extension && !strchr(basename, '.') && length + strlen(extension) < sizeof(path)) {
        strcpy(path + length, extension);
        if (!lstat(path, &info) && S_ISREG(info.st_mode))
            return win_GetFullPathNameA(path, capacity, output, part);
    }
    last_error = 2;
    return 0;
}

typedef struct {
    char *module_name;
    void *file, *mapped, *nt_header, *last_section;
    DWORD section_count;
    void *sections;
    DWORD characteristics;
    unsigned char system_image, dos_image, read_only, version;
    void *links[2];
    DWORD size;
} LoadedImage;
_Static_assert(offsetof(LoadedImage, size) == 80, "IMAGEHLP x64 layout");

static unsigned char *checked_nt(unsigned char *image, size_t size)
{
    if (!image || size < 64 || read_word(image) != 0x5a4d)
        return NULL;
    size_t offset = read_dword(image + 60);
    if (offset < 64 || offset > size || size - offset < 26)
        return NULL;
    unsigned char *nt = image + offset;
    unsigned optional_size = read_word(nt + 20), magic = read_word(nt + 24);
    if (read_dword(nt) != 0x4550 || size - offset - 24 < optional_size ||
        !((magic == 0x10b && optional_size == 224) || (magic == 0x20b && optional_size == 240)) ||
        (size - offset - 24 - optional_size) / 40 < read_word(nt + 6))
        return NULL;
    return nt;
}

static LoadedImage *current_image;

WIN int win_MapAndLoad(const char *name, const char *directory, LoadedImage *output,
                       int dot_dll, int read_only)
{
    (void)directory; (void)dot_dll;
    char path[1040];
    struct stat info;
    if (!read_only || current_image || !output || !exelist_path(name, path, sizeof(path))) {
        last_error = 87;
        return 0;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        last_error = 2;
        return 0;
    }
    unsigned char *data = NULL;
    unsigned char *nt = NULL;
    if (!fstat(fd, &info) && S_ISREG(info.st_mode) && info.st_size >= 64 &&
        info.st_size <= 64 * 1024 * 1024) {
        data = malloc(info.st_size);
        if (data && pread(fd, data, info.st_size, 0) == info.st_size)
            nt = checked_nt(data, info.st_size);
    }
    close(fd);
    if (!nt) {
        free(data);
        last_error = 11;
        return 0;
    }
    char *module = strdup(name);
    if (!module) {
        free(data);
        last_error = 8;
        return 0;
    }
    // Version resources are not exposed to the guest's unbounded PE walker.
    unsigned resource_entry = (read_word(nt + 24) == 0x20b ? 112 : 96) + 24 + 16;
    memset(nt + resource_entry, 0, 8);
    *output = (LoadedImage){.module_name = module, .mapped = data, .nt_header = nt,
        .section_count = read_word(nt + 6), .sections = nt + 24 + read_word(nt + 20),
        .characteristics = read_word(nt + 22), .read_only = 1, .size = info.st_size};
    current_image = output;
    last_error = 0;
    return 1;
}

WIN void *win_ImageNtHeader(const void *image)
{
    return current_image && image == current_image->mapped ? current_image->nt_header : NULL;
}

WIN int win_UnMapAndLoad(LoadedImage *image)
{
    if (!image || image != current_image)
        return 0;
    free(image->mapped);
    free(image->module_name);
    current_image = NULL;
    memset(image, 0, sizeof(*image));
    return 1;
}

WIN int win_GetBinaryTypeA(const char *name, DWORD *type)
{
    (void)name;
    if (!current_image || !type)
        return 0;
    const unsigned char *nt = current_image->nt_header;
    *type = read_word(nt + 24) == 0x20b ? 6 : 0;
    return 1;
}

WIN void win_RtlSecondsSince1970ToTime(DWORD seconds, uint64_t *output)
{
    *output = 116444736000000000ull + (uint64_t)seconds * 10000000;
}

WIN int win_CompareFileTime(const uint64_t *a, const uint64_t *b)
{
    return *a == *b ? 0 : *a < *b ? -1 : 1;
}
