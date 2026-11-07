#include <ctype.h>
#include <glob.h>
#include <locale.h>
#include <wchar.h>

WIN void *win_realloc(void *memory, size_t size) { return realloc(memory, size); }
WIN void win_free(void *memory) { free(memory); }
WIN void *win_memcpy(void *dst, const void *src, size_t size) { return memcpy(dst, src, size); }
WIN char *win_strncpy(char *dst, const char *src, size_t size) { return strncpy(dst, src, size); }
WIN const char *win_strchr(const char *text, int ch) { return strchr(text, ch); }
WIN const char *win_strrchr(const char *text, int ch) { return strrchr(text, ch == '\\' ? '/' : ch); }
WIN int win_strncmp(const char *a, const char *b, size_t size) { return strncmp(a, b, size); }
WIN int win_isdigit(int ch) { return ch >= '0' && ch <= '9'; }
WIN char *win_getenv(const char *name) { return getenv(name); }
WIN char *win_strerror(int error) { return strerror(error); }
WIN int *win__errno(void) { return &errno; }

WIN int32_t win_strtol(const char *text, char **end, int base)
{
    long value = strtol(text, end, base);
    if (value > INT32_MAX || value < INT32_MIN) {
        errno = ERANGE;
        return value < 0 ? INT32_MIN : INT32_MAX;
    }
    return value;
}

WIN char *win_setlocale(int category, const char *locale)
{
    return category == 0 ? setlocale(LC_ALL, locale) : NULL;
}
WIN int win_mblen(const char *text, size_t size)
{
    // The guest retries at the same address with more bytes after -1.
    mbstate_t state = {0};
    size_t length = mbrlen(text, size, &state);
    return length == (size_t)-1 || length == (size_t)-2 ? -1 : (int)length;
}

WIN FILE *win_fopen(const char *path, const char *mode)
{
    if (strcmp(mode, "r") && strcmp(mode, "rb")) {
        errno = EINVAL;
        return NULL;
    }
    return fopen(path, "rb");
}
WIN int win_fclose(void *stream) { return fclose(raw_stream(stream)); }
static int cut_text_character(FILE *file)
{
    int character = getc(file);
    if (character != '\r')
        return character;
    int next = getc(file);
    if (next == '\n')
        return next;
    if (next != EOF)
        ungetc(next, file);
    return character;
}
WIN int win_getc(void *stream) { return cut_text_character(raw_stream(stream)); }
WIN int win_putchar(int ch) { return putchar(ch); }
WIN char *win_fgets(char *text, int size, void *stream)
{
    FILE *file = raw_stream(stream);
    int count = 0;
    while (count < size - 1) {
        int character = cut_text_character(file);
        if (character == EOF)
            break;
        text[count++] = character;
        if (character == '\n')
            break;
    }
    if (!count || ferror(file))
        return NULL;
    text[count] = 0;
    return text;
}
WIN size_t win_fwrite(const void *data, size_t size, size_t count, void *stream)
{
    return fwrite(data, size, count, raw_stream(stream));
}
WIN int win_vfprintf(void *stream, const char *format, __builtin_ms_va_list args)
{
    return console_format(raw_stream(stream), format, args);
}

typedef struct {
    uint32_t attributes;
    int64_t created, accessed, modified;
    uint32_t size;
    char name[260];
} FindData32;
_Static_assert(offsetof(FindData32, name) == 36 && sizeof(FindData32) == 296, "MSVCRT finddata layout");

typedef struct {
    glob_t glob;
    size_t next;
} CutSearch;

WIN int win__findnext(CutSearch *search, FindData32 *data)
{
    while (search->next < search->glob.gl_pathc) {
        const char *path = search->glob.gl_pathv[search->next++];
        const char *name = strrchr(path, '/');
        name = name ? name + 1 : path;
        struct stat info;
        if (strlen(name) >= sizeof(data->name) || stat(path, &info) || !S_ISREG(info.st_mode))
            continue;
        memset(data, 0, sizeof(*data));
        data->size = info.st_size;
        memcpy(data->name, name, strlen(name) + 1);
        return 0;
    }
    errno = ENOENT;
    return -1;
}
WIN int win__findclose(CutSearch *search)
{
    globfree(&search->glob);
    free(search);
    return 0;
}
WIN CutSearch *win__findfirst(const char *pattern, FindData32 *data)
{
    CutSearch *search = calloc(1, sizeof(*search));
    if (!search)
        return (CutSearch *)-1;
    if (!glob(pattern, GLOB_NOESCAPE, NULL, &search->glob) && !win__findnext(search, data))
        return search;
    win__findclose(search);
    errno = ENOENT;
    return (CutSearch *)-1;
}
