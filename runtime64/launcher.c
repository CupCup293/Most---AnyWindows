#define _GNU_SOURCE
#include <limits.h>
#ifdef MOST_RUFUS
#include <systemd/sd-bus.h>
#endif
#include <dirent.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/sysmacros.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef MOST_W32VER
#define GUEST_MOUNT "/w32ver"
#else
#define GUEST_MOUNT "/rufus"
#endif

static int executable_path(char path[PATH_MAX])
{
    const ssize_t size = readlink("/proc/self/exe", path, PATH_MAX - 1);
    if (size < 0 || size >= PATH_MAX - 1)
        return 0;
    path[size] = 0;
    return 1;
}

int native_launch;
int native_usb_fd = -1;
int native_usb_authorized;
const char *native_executable;
char native_workdir[] = "/tmp/most-native-XXXXXX";

#ifndef MOST_W32VER
static int removable_usb(dev_t device)
{
    char link[PATH_MAX], path[PATH_MAX], attribute[PATH_MAX];
    snprintf(link, sizeof(link), "/sys/dev/block/%u:%u", major(device), minor(device));
    if (!realpath(link, path) || strlen(path) + sizeof("/subsystem") >= sizeof(attribute))
        return 0;
    snprintf(attribute, sizeof(attribute), "%s/partition", path);
    if (access(attribute, F_OK) == 0)
        return 0;
    snprintf(attribute, sizeof(attribute), "%s/removable", path);
    FILE *file = fopen(attribute, "r");
    if (!file)
        return 0;
    int removable = fgetc(file) == '1';
    fclose(file);
    if (!removable)
        return 0;
    for (char *end; (end = strrchr(path, '/')) && end != path;) {
        *end = 0;
        snprintf(attribute, sizeof(attribute), "%s/subsystem", path);
        if (realpath(attribute, link) && !strcmp(link, "/sys/bus/usb"))
            return 1;
    }
    return 0;
}

static int usb_candidate(const char *name, struct stat *info)
{
    return !strncmp(name, "/dev/", 5) && stat(name, info) == 0 &&
           S_ISBLK(info->st_mode) && removable_usb(info->st_rdev);
}

static int list_usb(void)
{
    DIR *directory = opendir("/dev/disk/by-id");
    if (!directory) {
        perror("/dev/disk/by-id");
        return 124;
    }
    for (struct dirent *entry; (entry = readdir(directory));) {
        if (strncmp(entry->d_name, "usb-", 4))
            continue;
        char name[PATH_MAX];
        struct stat info;
        snprintf(name, sizeof(name), "/dev/disk/by-id/%s", entry->d_name);
        if (usb_candidate(name, &info))
            printf("%s%s\n", name, access(name, R_OK | W_OK) ? " (no read/write permission)" : "");
    }
    closedir(directory);
    return 0;
}

#ifdef MOST_RUFUS
static int authorize_usb(const char *path)
{
    const char *device = strrchr(path, '/') + 1;
    if (!*device || strspn(device, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") != strlen(device))
        return -1;
    char object[PATH_MAX];
    if (snprintf(object, sizeof(object), "/org/freedesktop/UDisks2/block_devices/%s", device) >= (int)sizeof(object))
        return -1;

    sd_bus *bus = NULL;
    sd_bus_message *reply = NULL;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    int fd = -1;
    int result = sd_bus_open_system(&bus);
    if (result >= 0)
        result = sd_bus_set_allow_interactive_authorization(bus, 1);
    if (result >= 0)
        result = sd_bus_set_method_call_timeout(bus, 120000000);
    if (result >= 0)
        result = sd_bus_call_method(bus, "org.freedesktop.UDisks2", object,
            "org.freedesktop.UDisks2.Block", "OpenDevice", &error, &reply,
            "sa{sv}", "rw", 1, "flags", "i", O_EXCL | O_CLOEXEC | O_NONBLOCK);
    if (result >= 0) {
        int received;
        result = sd_bus_message_read(reply, "h", &received);
        if (result > 0)
            fd = fcntl(received, F_DUPFD_CLOEXEC, 3);
    }
    if (fd < 0)
        fprintf(stderr, "UDisks could not authorize access to the USB disk: %s\n",
                error.message ? error.message : result < 0 ? strerror(-result) : "invalid device descriptor");
    sd_bus_error_free(&error);
    sd_bus_message_unref(reply);
    sd_bus_unref(bus);
    return fd;
}
#endif

static int claim_usb(const char *name, int authorize)
{
    char path[PATH_MAX];
    struct stat expected, opened;
    if (strncmp(name, "/dev/disk/by-id/usb-", 20) || !usb_candidate(name, &expected) ||
        !realpath(name, path)) {
        fputs("Select a whole removable USB disk from --list-usb.\n", stderr);
        return 0;
    }
    int fd;
#ifdef MOST_RUFUS
    if (authorize) {
        fd = authorize_usb(path);
        if (fd < 0)
            return 0;
    } else
#else
    (void)authorize;
#endif
        fd = open(path, O_RDWR | O_EXCL | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        perror("Cannot exclusively open USB disk");
        return 0;
    }
    int sector = 0;
    uint64_t size = 0;
    if ((fcntl(fd, F_GETFL) & O_ACCMODE) != O_RDWR ||
        fstat(fd, &opened) || !S_ISBLK(opened.st_mode) || opened.st_rdev != expected.st_rdev ||
        !usb_candidate(name, &opened) || opened.st_rdev != expected.st_rdev ||
        ioctl(fd, BLKSSZGET, &sector) || sector != 512 ||
        ioctl(fd, BLKGETSIZE64, &size) || size < 16 * 1024 * 1024) {
        close(fd);
        fputs("USB identity or geometry could not be verified (requires 512-byte sectors).\n", stderr);
        return 0;
    }
    native_usb_fd = fd;
    native_usb_authorized = authorize;
    fprintf(stderr, "Selected USB disk: %s (%s, %llu bytes).\n",
            name, path, (unsigned long long)size);
    return 1;
}
#endif

#if defined(MOST_REPTXT) || defined(MOST_ELTORITO) || defined(MOST_EXELIST)
static int pass_absolute_path(char **argument)
{
    if (**argument != '/' || !strcmp(*argument, "/?"))
        return 1;
    char *windows = malloc(strlen(*argument) + 3);
    if (!windows)
        return 0;
    sprintf(windows, "Z:%s", *argument);
    *argument = windows;
    return 1;
}
#endif

int launch_if_needed(int argc, char **argv)
{
#ifdef MOST_RAWCOPY
    int offset = 0, comparison = 0;
    for (int i = 1; i < argc && argv[i][0] == '-' && argv[i][1]; ++i) {
        const char *option = argv[i] + 1;
        for (; *option && *option != ':'; ++option) {
            offset |= *option == 'o';
            comparison |= *option == 'D' || *option == 's';
        }
    }
    if (offset && comparison) {
        fputs("RawCopy cannot combine a write offset with differential or sparse mode.\n", stderr);
        return 124;
    }
#endif
#ifdef MOST_FSPLIT
    // The guest appends a decimal part number in its fixed 260-byte buffer.
    for (int i = 1; i < argc && i <= 2; ++i) {
        if (strlen(argv[i]) > 248) {
            fputs("FSplit paths must be at most 248 bytes.\n", stderr);
            return 124;
        }
    }
#endif
#ifdef MOST_FILEINFO
    if (argc > 1 && (argv[1][0] == '+' || argv[1][0] == '-') &&
        argv[1][1] >= '0' && argv[1][1] <= '9') {
        fputs("FileInfo can inspect attributes; changing them is unavailable.\n", stderr);
        return 1;
    }
#endif
#ifdef MOST_REPTXT
    for (int i = 1; i < argc; ++i) {
        if (strlen(argv[i]) > 240) {
            fputs("Reptxt arguments must be at most 240 bytes.\n", stderr);
            return 124;
        }
    }
    int path = 1;
    while (path < argc && (!strcasecmp(argv[path], "/A") || !strcasecmp(argv[path], "/I")))
        ++path;
    if (argc - path == 3 && (!*argv[path + 1] || strlen(argv[path + 1]) != strlen(argv[path + 2]))) {
        fputs("Reptxt requires nonempty search and replacement strings of equal byte length.\n", stderr);
        return 1;
    }
    if (path < argc && !pass_absolute_path(&argv[path]))
        return 124;
#endif
#ifdef MOST_ELTORITO
    if (argc == 2 && !pass_absolute_path(&argv[1]))
        return 124;
#endif
#ifdef MOST_EXELIST
    for (int i = 1; i < argc; ++i)
        if (!pass_absolute_path(&argv[i]))
            return 124;
#endif
#if defined(MOST_RAWCOPY) || defined(MOST_CHSIZE) || defined(MOST_FSPLIT) || defined(MOST_FILEINFO) || defined(MOST_REPTXT) || defined(MOST_ELTORITO) || defined(MOST_CUT) || defined(MOST_GETHOST) || defined(MOST_EXELIST)
    return -1; // Console profiles operate on files selected by their arguments.
#endif
    char executable[PATH_MAX];
    if (!executable_path(executable))
        return 124;
    if (!strcmp(executable, GUEST_MOUNT))
        return -1;
#ifndef MOST_W32VER
    if (argc == 2 && !strcmp(argv[1], "--list-usb"))
        return list_usb();
#endif
    if (argc >= 2 && !strcmp(argv[1], "--native")) {
        if (geteuid() == 0) {
            fputs("Run the native application as a regular user.\n", stderr);
            return 124;
        }
#ifndef MOST_W32VER
        if (argc == 4 && !strcmp(argv[2], "--usb")) {
            if (!claim_usb(argv[3], 0))
                return 124;
#ifdef MOST_RUFUS
        } else if (argc == 4 && !strcmp(argv[2], "--authorize-usb")) {
            if (!claim_usb(argv[3], 1))
                return 124;
#endif
        } else
#endif
        if (argc != 2) {
            fputs("Invalid arguments for --native.\n", stderr);
            return 124;
        }
        extern char **environ;
        for (char **entry = environ; *entry;) {
            if (!strncmp(*entry, "RUFUS_PROBE_", 12)) {
                char *equals = strchr(*entry, '=');
                if (equals) {
                    char name[128];
                    size_t length = equals - *entry;
                    if (length >= sizeof(name))
                        return 124;
                    memcpy(name, *entry, length);
                    name[length] = 0;
                    unsetenv(name);
                    continue;
                }
            }
            ++entry;
        }
        if (!mkdtemp(native_workdir) || chdir(native_workdir)) {
            perror("Cannot create a private working directory");
            return 124;
        }
        native_executable = strdup(executable);
        if (!native_executable)
            return 124;
        native_launch = 1;
        return -1;
    }
#ifdef MOST_W32VER
    if (argc != 1) {
        fputs("Usage: w32ver-linux\n", stderr);
        return 124;
    }
#endif
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    const char *display = getenv("WAYLAND_DISPLAY");
    char socket[PATH_MAX];
    struct stat info;
    if (!runtime || !display || strchr(display, '/') ||
        snprintf(socket, sizeof(socket), "%s/%s", runtime, display) >= (int)sizeof(socket) ||
        stat(socket, &info) || !S_ISSOCK(info.st_mode)) {
        fputs("This relinked application requires a Wayland graphical session.\n", stderr);
        return 124;
    }

    char image[PATH_MAX], image_mount[PATH_MAX], device[PATH_MAX];
    struct stat image_info = {0}, device_info = {0}, executable_info;
    int mount_image = 0, mount_device = 0;
    for (int index = 1; index < argc; index += 2) {
        if (index + 1 == argc) {
            fputs("Usage: rufus-linux [--image FILE] [--target-image FILE.img]\n", stderr);
            return 124;
        }
        if (!strcmp(argv[index], "--image") && !mount_image) {
            if (!realpath(argv[index + 1], image) || stat(image, &image_info) ||
                !S_ISREG(image_info.st_mode)) {
                fputs("Could not open the selected image file.\n", stderr);
                return 124;
            }
            // Keep the guest path short and ASCII. Rufus uses the suffix for compressed formats.
            const char *name = strrchr(argv[index + 1], '/');
            name = name ? name + 1 : argv[index + 1];
            const char *suffix = strrchr(name, '.');
            const char *letters = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
            if (!suffix || !suffix[1] || strlen(suffix) > 17 ||
                strspn(suffix + 1, letters) != strlen(suffix + 1))
                suffix = ".img";
            snprintf(image_mount, sizeof(image_mount), "/scratch/source%s", suffix);
            mount_image = 1;
        } else if (!strcmp(argv[index], "--target-image") && !mount_device) {
            if (!realpath(argv[index + 1], device) || stat(device, &device_info) ||
                !S_ISREG(device_info.st_mode) || device_info.st_size < 16 * 1024 * 1024 ||
                strlen(device) < 4 || strcmp(device + strlen(device) - 4, ".img") ||
                access(device, W_OK)) {
                fputs("The target must be a writable existing .img file of at least 16 MiB.\n", stderr);
                return 124;
            }
            mount_device = 1;
        } else {
            fputs("Usage: rufus-linux [--image FILE] [--target-image FILE.img]\n", stderr);
            return 124;
        }
    }
    if (mount_device &&
        ((mount_image && image_info.st_ino == device_info.st_ino && image_info.st_dev == device_info.st_dev) ||
         stat(executable, &executable_info) ||
         (executable_info.st_ino == device_info.st_ino && executable_info.st_dev == device_info.st_dev))) {
        fputs("The target must differ from the input and the executable.\n", stderr);
        return 124;
    }

    char *arguments[80] = {
        "bwrap", "--unshare-all", "--uid", "0", "--gid", "0", "--new-session", "--clearenv",
        "--setenv", "QT_QPA_PLATFORM", "wayland", "--setenv", "WAYLAND_DISPLAY", "compositor",
        "--setenv", "XDG_RUNTIME_DIR", "/runtime", "--setenv", "QT_WAYLAND_CLIENT_BUFFER_INTEGRATION", "shm",
        "--setenv", "HOME", "/scratch", "--setenv", "XDG_CACHE_HOME", "/scratch/cache",
        "--perms", "0700", "--dir", "/runtime", "--ro-bind", socket, "/runtime/compositor",
        "--ro-bind-try", "/etc/fonts", "/etc/fonts", "--ro-bind", "/usr", "/usr",
        "--ro-bind", "/lib", "/lib", "--ro-bind", "/lib64", "/lib64",
        "--ro-bind", executable, GUEST_MOUNT, "--tmpfs", "/scratch", "--proc", "/proc",
        "--chdir", "/scratch", NULL
    };
    size_t i = 0;
    while (arguments[i])
        ++i;
    if (mount_image) {
        arguments[i++] = "--ro-bind";
        arguments[i++] = image;
        arguments[i++] = image_mount;
        arguments[i++] = "--setenv";
        arguments[i++] = "MOST_RUFUS_IMAGE";
        arguments[i++] = image_mount;
    }
    if (mount_device) {
        arguments[i++] = "--bind";
        arguments[i++] = device;
        arguments[i++] = "/scratch/device.img";
        arguments[i++] = "--setenv";
        arguments[i++] = "RUFUS_PROBE_DEVICE";
        arguments[i++] = "1";
    }
    arguments[i++] = "--";
    arguments[i++] = GUEST_MOUNT;
    arguments[i] = NULL;
    execvp("bwrap", arguments);
    perror("bwrap");
    return 124;
}
