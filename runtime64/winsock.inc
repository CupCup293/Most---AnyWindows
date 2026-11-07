#include <arpa/inet.h>
#include <netdb.h>

typedef struct {
    uint16_t version, high_version, max_sockets, max_datagram;
    char *vendor;
    char description[257], status[129];
} WsaData;
_Static_assert(sizeof(WsaData) == 408, "Winsock x64 WSADATA layout");

typedef struct {
    char *name;
    char **aliases;
    int16_t type, address_length;
    char **addresses;
} WinHostEntry;
_Static_assert(offsetof(WinHostEntry, addresses) == 24, "Winsock x64 HOSTENT layout");

static _Thread_local WinHostEntry winsock_host;

static WinHostEntry *guest_host_entry(const struct hostent *entry)
{
    if (!entry || entry->h_addrtype != AF_INET || entry->h_length != sizeof(struct in_addr))
        return NULL;
    winsock_host = (WinHostEntry){entry->h_name, entry->h_aliases, 2,
                                  sizeof(struct in_addr), entry->h_addr_list};
    return &winsock_host;
}

// Ordinal assignments are from the WSOCK32 export table.
WIN int win_wsock32_ordinal_115(uint16_t requested, WsaData *data)
{
    if (!data)
        return 10014; // WSAEFAULT
    if (requested != 0x0101)
        return 10092; // WSAVERNOTSUPPORTED
    *data = (WsaData){.version = 0x0101, .high_version = 0x0101,
                      .description = "Most Winsock 1.1 host lookup", .status = "Running"};
    return 0;
}

WIN int win_wsock32_ordinal_57(char *name, int size)
{
    return name && size > 0 ? gethostname(name, (size_t)size) : -1;
}

WIN uint32_t win_wsock32_ordinal_10(const char *address) { return inet_addr(address); }

WIN const char *win_wsock32_ordinal_11(struct in_addr address) { return inet_ntoa(address); }

WIN WinHostEntry *win_wsock32_ordinal_52(const char *name)
{
    return name ? guest_host_entry(gethostbyname(name)) : NULL;
}

WIN WinHostEntry *win_wsock32_ordinal_51(const void *address, int length, int type)
{
    if (!address || length != sizeof(struct in_addr) || type != 2)
        return NULL;
    return guest_host_entry(gethostbyaddr(address, (socklen_t)length, AF_INET));
}
