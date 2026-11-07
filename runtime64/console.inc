#include <malloc.h>

/* Microsoft x64 msvcrt FILE objects are 48 bytes. Only the standard streams
   are passed to this bridge. Keep host FILE objects behind the ABI boundary. */
unsigned char win__iob[3][48] __attribute__((aligned(8)));

static FILE *raw_stream(const void *stream)
{
    if (stream == win__iob[0]) return stdin;
    if (stream == win__iob[1]) return stdout;
    if (stream == win__iob[2]) return stderr;
#ifdef MOST_CUT
    return (FILE *)stream;
#else
    return NULL;
#endif
}

#if defined(MOST_RAWCOPY) || defined(MOST_FILEINFO)
WIN WCHAR **win_CommandLineToArgvW(const WCHAR *line, int *count)
{
    if (line != win_GetCommandLineW() || !count)
        return NULL;
    size_t size = (guest_argc + 1) * sizeof(WCHAR *);
    for (int i = 0; i < guest_argc; ++i)
        size += (wide_length(guest_argv[i]) + 1) * sizeof(WCHAR);
    WCHAR **result = malloc(size);
    if (!result)
        return NULL;
    WCHAR *text = (WCHAR *)(result + guest_argc + 1);
    for (int i = 0; i < guest_argc; ++i) {
        size_t chars = wide_length(guest_argv[i]) + 1;
        result[i] = text;
        memcpy(text, guest_argv[i], chars * sizeof(WCHAR));
        text += chars;
    }
    result[guest_argc] = NULL;
    *count = guest_argc;
    return result;
}
#endif

#ifdef MOST_RAWCOPY
WIN void *win_LocalAlloc(DWORD flags, size_t size)
{
    if (flags & 2) return NULL;
    return flags & 0x40 ? calloc(1, size ? size : 1) : malloc(size ? size : 1);
}
WIN size_t win_LocalSize(void *memory) { return memory ? malloc_usable_size(memory) : 0; }
WIN DWORD win_SetErrorMode(DWORD mode)
{
    static DWORD current;
    DWORD old = current;
    current = mode;
    return old;
}

#endif

WIN int win_puts(const char *text) { return puts(text); }

WIN int win_fputs(const char *text, const void *stream)
{
    FILE *file = raw_stream(stream);
    return file ? fputs(text, file) : EOF;
}
WIN int win_fflush(const void *stream)
{
    return !stream ? fflush(NULL) : raw_stream(stream) ? fflush(raw_stream(stream)) : EOF;
}
WIN void win_exit(int status)
{
#ifdef MOST_FILEINFO
    if (!status && fileinfo_failed)
        status = 1;
#elif defined(MOST_REPTXT)
    if (!status && reptxt_failed)
        status = 1;
#endif
    exit(status);
}
#if defined(MOST_RAWCOPY) || defined(MOST_ELTORITO) || defined(MOST_EXELIST)
WIN int win_memcmp(const void *a, const void *b, size_t size) { return memcmp(a, b, size); }
#endif

WIN int win_CharToOemA(const char *source, char *destination)
{
    memmove(destination, source, strlen(source) + 1);
    return 1;
}

WIN size_t win_RtlCompareMemoryUlong(const void *memory, size_t size, DWORD value)
{
    const unsigned char *bytes = memory;
    size_t index = 0;
    for (; size - index >= sizeof(value); index += sizeof(value))
        if (memcmp(bytes + index, &value, sizeof(value)))
            break;
    return index;
}

WIN unsigned long win_wcstoul(const WCHAR *text, WCHAR **end, int base)
{
    size_t size = wide_length(text);
    char *narrow = malloc(size + 1);
    if (!narrow)
        return 0;
    for (size_t i = 0; i < size; ++i)
        narrow[i] = text[i] <= 127 ? text[i] : '?';
    narrow[size] = 0;
    char *stop;
    unsigned long value = strtoul(narrow, &stop, base);
    if (end)
        *end = (WCHAR *)text + (stop - narrow);
    free(narrow);
    return value > UINT32_MAX ? UINT32_MAX : value;
}

#if defined(MOST_RAWCOPY) || defined(MOST_CHSIZE)
static int scan_size(const char *text, long long *value, char *suffix)
{
    char *end;
    errno = 0;
    long long number = strtoll(text, &end, 0);
    if (end == text || errno == ERANGE)
        return 0;
    *value = number;
    if (!*end)
        return 1;
    *suffix = *end;
    return 2;
}

#ifdef MOST_RAWCOPY
WIN int win_swscanf(const WCHAR *text, const WCHAR *format, long long *value, WCHAR *suffix)
{
    static const WCHAR expected[] = u"%I64i%c";
    if (memcmp(format, expected, sizeof(expected)))
        return -1;
    size_t length = wide_length(text);
    if (length >= 128)
        return -1;
    char narrow[128], tail;
    for (size_t i = 0; i <= length; ++i)
        narrow[i] = text[i] <= 127 ? text[i] : '?';
    int count = scan_size(narrow, value, &tail);
    if (count == 2)
        *suffix = (unsigned char)tail;
    return count;
}
#endif

#endif

#if defined(MOST_CHSIZE) || defined(MOST_FSPLIT) || defined(MOST_REPTXT) || defined(MOST_ELTORITO) || defined(MOST_CUT) || defined(MOST_GETHOST) || defined(MOST_EXELIST)
int win__commode, win__fmode;
char **win___initenv;
extern char **guest_narrow_argv;

#ifdef MOST_CUT
char **win___argv;
#endif

WIN int win___getmainargs(int *argc, char ***argv, char ***environment, int wildcard, void *startup)
{
    (void)startup;
    static char *empty_environment[] = {NULL};
    if (wildcard || !argc || !argv || !environment)
        return -1;
    *argc = guest_argc;
    *argv = guest_narrow_argv;
#ifdef MOST_CUT
    win___argv = *argv;
#endif
    *environment = empty_environment;
    return 0;
}

typedef WIN void (*Initializer)(void);
WIN void win__initterm(Initializer *first, Initializer *end)
{
    for (; first < end; ++first)
        if (*first)
            (*first)();
}
WIN void win___set_app_type(int type) { (void)type; }
WIN void win___setusermatherr(void *handler) { (void)handler; }
WIN void win__c_exit(void) {}
WIN void win__cexit(void) { fflush(NULL); }
WIN void win__exit(int status) { _Exit(status); }
WIN int win__strcmpi(const char *a, const char *b) { return strcasecmp(a, b); }

#endif

#ifdef MOST_CHSIZE
WIN int win_sscanf(const char *text, const char *format, long long *value, char *suffix)
{
    if (strcmp(format, "%I64i%c"))
        return -1;
    long long number;
    char tail;
    int count = scan_size(text, &number, &tail);
    if (!count || number < 0)
        return 0;
    if (count == 2) {
        const char *units = "kmgt";
        const char *unit = strchr(units, ascii_fold((unsigned char)tail));
        if (!unit ||
            number > (LLONG_MAX >> (10 * (unit - units + 1))))
            return 0;
        // Require exactly one suffix; sscanf would otherwise ignore the rest.
        char *end;
        strtoll(text, &end, 0);
        if (end[1])
            return 0;
        *suffix = tail;
    }
    *value = number;
    return count;
}
#endif

static int console_format(FILE *file, const char *format, __builtin_ms_va_list args)
{
    int total = 0;
    while (*format && total >= 0) {
        if (*format != '%') {
            total = fputc(*format++, file) == EOF ? EOF : total + 1;
            continue;
        }
        char spec[32] = "%";
        size_t length = 1;
        ++format;
        while (*format && strchr("0123456789.-+ #", *format) && length < sizeof(spec) - 5)
            spec[length++] = *format++;
        if (length == 2 && spec[1] == '.' && !strncmp(format, "*s", 2)) {
            int precision = __builtin_va_arg(args, int);
            const char *text = __builtin_va_arg(args, const char *);
            int written = fprintf(file, "%.*s", precision, text);
            total = written < 0 ? EOF : total + written;
            format += 2;
            continue;
        }
        int large = !strncmp(format, "I64", 3);
        int wide = *format == 'w';
        if (large) {
            spec[length++] = 'l';
            spec[length++] = 'l';
            format += 3;
        } else if (wide) {
            ++format;
        }
        char type = *format ? *format++ : 0;
        spec[length++] = type;
        spec[length] = 0;
        int written = EOF;
        if (!large && !wide && type == '%')
            written = fputc('%', file) == EOF ? EOF : 1;
        else if (!large && type == 's' && wide) {
            const WCHAR *text = __builtin_va_arg(args, const WCHAR *);
            int size = text ? win_WideCharToMultiByte(65001, 0, text, -1, NULL, 0, NULL, NULL) : 0;
            char *value = size > 0 ? malloc(size) : NULL;
            if (value && win_WideCharToMultiByte(65001, 0, text, -1, value, size, NULL, NULL))
                written = fprintf(file, spec, value);
            free(value);
        } else if (!large && !wide && type == 's')
            written = fprintf(file, spec, __builtin_va_arg(args, const char *));
        else if (type == 'c' && !large)
            written = fprintf(file, spec, __builtin_va_arg(args, int));
        else if (type && !wide && strchr("di", type))
            written = large ? fprintf(file, spec, __builtin_va_arg(args, long long))
                            : fprintf(file, spec, __builtin_va_arg(args, int));
        else if (type && !wide && strchr("uxX", type))
            written = large ? fprintf(file, spec, __builtin_va_arg(args, unsigned long long))
                            : fprintf(file, spec, __builtin_va_arg(args, unsigned));
        else if (!large && !wide && type == 'g')
            written = fprintf(file, spec, __builtin_va_arg(args, double));
        total = written < 0 ? EOF : total + written;
    }
    return total;
}

WIN int win_fprintf(const void *stream, const char *format, ...)
{
    FILE *file = raw_stream(stream);
    if (!file)
        return EOF;
    __builtin_ms_va_list args;
    __builtin_ms_va_start(args, format);
    int result = console_format(file, format, args);
    __builtin_ms_va_end(args);
    return result;
}

#if defined(MOST_FSPLIT) || defined(MOST_FILEINFO) || defined(MOST_REPTXT) || defined(MOST_GETHOST) || defined(MOST_EXELIST)
WIN int win_printf(const char *format, ...)
{
    __builtin_ms_va_list args;
    __builtin_ms_va_start(args, format);
    int result = console_format(stdout, format, args);
    __builtin_ms_va_end(args);
    return result;
}

#endif

#if defined(MOST_FSPLIT) || defined(MOST_ELTORITO)
WIN void win_perror(const char *message) { perror(message); }
#endif
#if defined(MOST_FSPLIT) || defined(MOST_ELTORITO) || defined(MOST_CUT)
WIN void *win_malloc(size_t size) { return malloc(size); }
#endif

#ifdef MOST_FSPLIT
WIN int win_sprintf(char *output, const char *format, unsigned part)
{
    // The only caller writes a decimal part number into the path suffix.
    return strcmp(format, "%u") ? -1 : snprintf(output, 11, "%u", part);
}

WIN char *win_strncpy(char *dst, const char *src, size_t n) { return strncpy(dst, src, n); }

WIN uint32_t win_strtoul(const char *text, char **end, int base)
{
    char *stop;
    errno = 0;
    unsigned long value = strtoul(text, &stop, base);
    if (end)
        *end = stop;
    return errno || *stop || value > INT32_MAX ? 0 : (uint32_t)value;
}

static struct stat split_output;
static int split_output_valid;

WIN int win__open(const char *path, int flags, ...)
{
    if (flags != 0x8020 && flags != 0x8121) {
        errno = EINVAL;
        return -1;
    }
    int output = flags == 0x8121;
    int fd = open(path, O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK |
                        (output ? O_WRONLY | O_CREAT | O_EXCL : O_RDONLY), 0600);
    if (fd < 0)
        return -1;
    struct stat info;
    if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_size > INT32_MAX) {
        close(fd);
        errno = EINVAL;
        return -1;
    }
    if (output) {
        split_output = info;
        split_output_valid = 1;
    }
    return fd;
}

static int split_transfer(int fd, void *buffer, unsigned count, int writing)
{
    unsigned done = 0;
    while (done < count) {
        char *next = (char *)buffer + done;
        ssize_t size = writing ? write(fd, next, count - done) : read(fd, next, count - done);
        if (size < 0 && errno == EINTR)
            continue;
        if (size < 0)
            return -1;
        if (!size) {
            if (!writing)
                break;
            errno = EIO;
            return -1;
        }
        done += size;
    }
    return done;
}
WIN int win__read(int fd, void *buffer, unsigned count) { return split_transfer(fd, buffer, count, 0); }
WIN int win__write(int fd, const void *buffer, unsigned count) { return split_transfer(fd, (void *)buffer, count, 1); }
WIN int win__close(int fd) { return close(fd); }

WIN int win__unlink(const char *path)
{
    struct stat info;
    if (!split_output_valid || lstat(path, &info) || info.st_dev != split_output.st_dev ||
        info.st_ino != split_output.st_ino) {
        errno = EACCES;
        return -1;
    }
    return unlink(path);
}

typedef struct {
    int32_t device;
    uint16_t inode, mode;
    int16_t links, uid, gid;
    int32_t special_device, size, access_time, modified_time, change_time;
} LegacyStat;
_Static_assert(offsetof(LegacyStat, size) == 20 && sizeof(LegacyStat) == 36, "MSVCRT stat layout");

WIN int win__fstat(int fd, LegacyStat *output)
{
    struct stat info;
    if (fstat(fd, &info))
        return -1;
    if (info.st_size > INT32_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    *output = (LegacyStat){.mode = info.st_mode, .size = info.st_size};
    return 0;
}
#endif

#if defined(MOST_FILEINFO) || defined(MOST_REPTXT) || defined(MOST_EXELIST)
WIN int win_fputc(int character, const void *stream)
{
    FILE *file = raw_stream(stream);
    return file ? fputc(character, file) : EOF;
}

#endif

#if defined(MOST_FILEINFO) || defined(MOST_EXELIST)
static int date_or_time(char *buffer, int capacity, const char *text, int length)
{
    if (length < 0 || capacity < 0 || (capacity && !buffer)) {
        last_error = 87;
        return 0;
    }
    if (!capacity)
        return length + 1;
    if (capacity <= length) {
        last_error = 122;
        return 0;
    }
    memcpy(buffer, text, length + 1);
    return length + 1;
}

WIN int win_GetTimeFormatA(DWORD locale, DWORD flags, const SystemTime *value, const char *format,
                           char *buffer, int capacity)
{
    (void)locale;
    if (!value || flags || format)
        return date_or_time(buffer, capacity, NULL, -1);
    char text[32];
    int length = snprintf(text, sizeof(text), "%02u:%02u:%02u", value->hour, value->minute, value->second);
    return date_or_time(buffer, capacity, text, length);
}

WIN int win_GetDateFormatA(DWORD locale, DWORD flags, const SystemTime *value, const char *format,
                           char *buffer, int capacity)
{
    (void)locale; (void)flags; (void)format;
    if (!value)
        return date_or_time(buffer, capacity, NULL, -1);
    char text[32];
    int length = snprintf(text, sizeof(text), "%04u-%02u-%02u", value->year, value->month, value->day);
    return date_or_time(buffer, capacity, text, length);
}

#endif

#ifdef MOST_FILEINFO
WIN int32_t win_NtSetInformationFile(uintptr_t file, void *status, const void *data, DWORD size, DWORD kind)
{
    (void)file; (void)status; (void)data; (void)size; (void)kind;
    return (int32_t)0xc0000022u; // The metadata profile is read-only.
}

WIN DWORD win_RtlNtStatusToDosError(int32_t status)
{
    return status == (int32_t)0xc0000022u ? 5 : 317;
}
#endif
