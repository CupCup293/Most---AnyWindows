/* Exercise the same Win64 volume functions used by the Rufus ELF. */
#include "../runtime64/startup.c"
#include <assert.h>

int native_launch, native_usb_authorized, native_usb_fd = -1;
const char *native_executable;
char native_workdir[PATH_MAX];

DWORD *guest_last_error(void)
{
    static DWORD error;
    return &error;
}

int main(void)
{
    const off_t start = 2048 * 512, length = 4096 * 512;
    unsigned char sector[512] = {0};
    sector[450] = 0x0c;
    sector[455] = 8;  // Starting LBA 2048
    sector[459] = 16; // Length 4096 sectors
    sector[510] = 0x55;
    sector[511] = 0xaa;

    int backing = open(probe_disk_path, O_RDWR);
    assert(backing >= 0);
    assert(pwrite(backing, sector, sizeof(sector), 0) == sizeof(sector));
    assert(pwrite(backing, "A", 1, start - 1) == 1);
    assert(pwrite(backing, "B", 1, start + length) == 1);

    WCHAR name[64];
    copy_path(probe_volume, name, 64);
    uintptr_t volume = win_CreateFileW(name, 0xc0000000u, 3, NULL, 3, 0, 0);
    uintptr_t disk = win_CreateFileW(u"\\\\.\\PhysicalDrive2", 0xc0000000u, 3, NULL, 3, 0, 0);
    assert(volume != UINTPTR_MAX && disk != UINTPTR_MAX);
    assert(win_SetVolumeMountPointA("C:\\", probe_volume));

    int64_t size, extent[4];
    DWORD done;
    assert(win_GetFileSizeEx(volume, &size) && size == length);
    assert(win_DeviceIoControl(volume, 0x560000, NULL, 0, extent, sizeof(extent), &done, NULL));
    assert(done == sizeof(extent) && extent[0] == 1 && extent[1] == 2 &&
           extent[2] == start && extent[3] == length);
    assert(!win_SetFilePointerEx(volume, -1, NULL, SEEK_SET));
    assert(!win_SetFilePointerEx(volume, 1, NULL, SEEK_END));

    assert(win_WriteFile(volume, "X", 1, &done, NULL) && done == 1);
    assert(win_SetFilePointerEx(volume, -1, NULL, SEEK_END));
    assert(!win_WriteFile(volume, "YZ", 2, &done, NULL) && done == 0);
    assert(win_WriteFile(volume, "Y", 1, &done, NULL) && done == 1);
    unsigned char bytes[2];
    assert(!win_ReadFile(volume, bytes, 1, &done, NULL) && done == 0);
    Overlapped request = {.offset = length - 1};
    assert(!win_ReadFile(volume, bytes, 2, &done, &request) && done == 0);
    assert(!win_WriteFile(volume, "YZ", 2, &done, &request) && done == 0);
    assert(win_ReadFile(volume, bytes, 1, &done, &request) && done == 1 && bytes[0] == 'Y');
    assert(pread(backing, bytes, 2, start - 1) == 2 && !memcmp(bytes, "AX", 2));
    assert(pread(backing, bytes, 2, start + length - 1) == 2 && !memcmp(bytes, "YB", 2));

    unsigned char raw_layout[24] = {2};
    assert(win_DeviceIoControl(disk, 0x7c058, raw_layout, sizeof(raw_layout), NULL, 0, &done, NULL));
    assert(win_DeviceIoControl(disk, 0x70140, NULL, 0, NULL, 0, &done, NULL));
    assert(win_FindFirstVolumeA((char *)bytes, sizeof(bytes)) == UINTPTR_MAX && last_error == 18);
    assert(!(win_GetLogicalDrives() & (1u << 2)));
    assert(win_WriteFile(disk, sector, sizeof(sector), &done, NULL));
    request.offset = 0;
    assert(!win_ReadFile(volume, bytes, 1, &done, &request) && last_error == 6);
    assert(!win_GetFileSizeEx(volume, &size));
    assert(win_CloseHandle(volume));
    volume = win_CreateFileW(name, 0x80000000u, 3, NULL, 3, 0, 0);
    assert(volume != UINTPTR_MAX && win_ReadFile(volume, bytes, 1, &done, NULL) && bytes[0] == 'X');
    assert(win_SetVolumeMountPointA("C:\\", probe_volume));
    assert(win_SetFilePointerEx(disk, 450, NULL, SEEK_SET));
    assert(win_WriteFile(disk, "\0", 1, &done, NULL));
    assert(!(win_GetLogicalDrives() & (1u << 2)));
    assert(!win_ReadFile(volume, bytes, 1, &done, NULL) && last_error == 6);
    assert(win_CloseHandle(volume) && win_CloseHandle(disk));
    assert(close(backing) == 0);
    puts("Logical volume bounds, layout invalidation and adjacent guard bytes verified");
}
