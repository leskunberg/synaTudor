#include <asm/prctl.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <stdarg.h>
#include <unistd.h>
#include "internal.h"

typedef struct _SYSTEM_INFO {
    union {
        DWORD dwOemId;
        struct {
            WORD wProcessorArchitecture;
            WORD wReserved;
        };
    };
    DWORD dwPageSize;
    void *lpMinimumApplicationAddress;
    void *lpMaximumApplicationAddress;
    ULONG_PTR dwActiveProcessorMask;
    DWORD dwNumberOfProcessors;
    DWORD dwProcessorType;
    DWORD dwAllocationGranularity;
    WORD wProcessorLevel;
    WORD wProcessorRevision;
} SYSTEM_INFO;

typedef struct {
    ULONG dwOSVersionInfoSize;
    ULONG dwMajorVersion;
    ULONG dwMinorVersion;
    ULONG dwBuildNumber;
    ULONG dwPlatformId;
    WCHAR szCSDVersion[128];
} OSVERSIONINFOW;

typedef struct {
    DWORD dwOSVersionInfoSize;
    DWORD dwMajorVersion;
    DWORD dwMinorVersion;
    DWORD dwBuildNumber;
    DWORD dwPlatformId;
    CHAR szCSDVersion[128];
    WORD wServicePackMajor;
    WORD wServicePackMinor;
    WORD wSuiteMask;
    BYTE wProductType;
    BYTE wReserved;
} OSVERSIONINFOEXA;

__winfnc NTSTATUS RtlGetVersion(OSVERSIONINFOW *ver) {
    ver->dwOSVersionInfoSize = sizeof(OSVERSIONINFOW);
    ver->dwMajorVersion = 10;
    ver->dwMinorVersion = 0;
    ver->dwBuildNumber = 17134;
    ver->dwPlatformId = 2; //VER_PLATFORM_WIN32_NT
    return 0;
}
WINAPI(RtlGetVersion)

__winfnc ULONGLONG VerSetConditionMask(ULONGLONG cond_mask, DWORD type_mask, BYTE condition) {
    return cond_mask;
}
WINAPI(VerSetConditionMask)

__winfnc void GetSystemInfo(SYSTEM_INFO *info) {
    info->wProcessorArchitecture = 9; //PROCESSOR_ARCHITECTURE_AMD64
    info->dwPageSize = 4096;
    info->lpMinimumApplicationAddress = (void*) 0;
    info->lpMaximumApplicationAddress = (void*) UINTPTR_MAX;
    info->dwActiveProcessorMask = 0b1;
    info->dwNumberOfProcessors = 1;
    info->dwProcessorType = 8664; //PROCESSOR_AMD_X8664
    info->dwAllocationGranularity = 8;
    info->wProcessorLevel = 0;
    info->wProcessorRevision = 0;
}
WINAPI(GetSystemInfo)

__winfnc BOOL VerifyVersionInfoW(ULONGLONG condition_mask, DWORD type_mask, ULONGLONG cond_mask) {
    //TODO
    return TRUE;
}
WINAPI(VerifyVersionInfoW)

__winfnc BOOL ConvertStringSecurityDescriptorToSecurityDescriptorW(const char16_t *str, DWORD rev, void *descrpt, ULONG *descrpt_size) {
    //TODO
    if(descrpt_size) *descrpt_size = 0;
    return TRUE;
}
WINAPI(ConvertStringSecurityDescriptorToSecurityDescriptorW)

__winfnc UINT GetSystemFirmwareTable(DWORD provider, DWORD table_id, void *buf, DWORD buf_size) {
    log_debug("GetSystemFirmwareTable called (provider=0x%x, table=0x%x, buf=%p, buf_size=%u)", provider, table_id, buf, buf_size);
    /*
     * SMBIOS (RSMB) raw data with Type 1 (System Information) for UUID.
     * Structure: 8-byte RawSMBIOSData header + SMBIOS table entries.
     * Each entry: fixed header (type+length+handle) + data + null-terminated strings + extra null.
     */
    static const uint8_t smbios_data[] = {
        /* === RawSMBIOSData header (8 bytes) === */
        0x00,       /* Used20CallingMethod */
        0x03,       /* SMBIOSMajorVersion */
        0x02,       /* SMBIOSMinorVersion */
        0x00,       /* DmiRevision */
        0x00, 0x00, 0x00, 0x00, /* Length - patched below */

        /* === Type 1: System Information (27 bytes header+data) === */
        0x01,       /* Type = 1 (System Information) */
        0x1B,       /* Length = 27 (SMBIOS 2.4 format) */
        0x01, 0x00, /* Handle = 0x0001 */
        0x01,       /* Manufacturer (string #1) */
        0x02,       /* Product Name (string #2) */
        0x03,       /* Version (string #3) */
        0x04,       /* Serial Number (string #4) */
        /* UUID (16 bytes) - fake but deterministic */
        0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0,
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
        0x06,       /* Wake-up Type = Power Switch */
        0x05,       /* SKU Number (string #5) */
        0x06,       /* Family (string #6) */
        /* Strings for Type 1 */
        'L','e','n','o','v','o',0,                     /* String #1: Manufacturer */
        'X','1',' ','F','o','l','d',0,                 /* String #2: Product */
        '1','.','0',0,                                 /* String #3: Version */
        'T','U','D','O','R','0','0','1',0,             /* String #4: Serial */
        'S','K','U','0','0','1',0,                     /* String #5: SKU */
        'T','h','i','n','k','P','a','d',0,             /* String #6: Family */
        0x00,       /* Extra null terminator (end of strings) */

        /* === Type 127: End of Table (4 bytes + double-null) === */
        0x7F,       /* Type = 127 (End-of-Table) */
        0x04,       /* Length = 4 */
        0xFF, 0xFF, /* Handle = 0xFFFF */
        0x00, 0x00  /* Double-null string terminator */
    };
    /* Compute table data length (everything after the 8-byte header) */
    static uint8_t patched_data[sizeof(smbios_data)];
    static int patched = 0;
    if(!patched) {
        memcpy(patched_data, smbios_data, sizeof(smbios_data));
        uint32_t table_len = sizeof(smbios_data) - 8;
        memcpy(patched_data + 4, &table_len, 4);
        patched = 1;
    }
    UINT total = sizeof(patched_data);
    if(!buf || buf_size < total) {
        winerr_set_code(122); /* ERROR_INSUFFICIENT_BUFFER */
        return total; /* Return required size */
    }
    memcpy(buf, patched_data, total);
    return total;
}
WINAPI(GetSystemFirmwareTable)

__winfnc DWORD GetTimeZoneInformation(void *tz_info) {
    if(tz_info) memset(tz_info, 0, 172); /* TIME_ZONE_INFORMATION is 172 bytes */
    return 0; /* TIME_ZONE_ID_UNKNOWN */
}
WINAPI(GetTimeZoneInformation)

__winfnc DWORD GetSystemDirectoryA(char *buf, UINT size) {
    const char *sysdir = "C:\\Windows\\System32";
    DWORD len = strlen(sysdir);
    if(buf && size > len) {
        strcpy(buf, sysdir);
        return len;
    }
    return len + 1;
}
WINAPI(GetSystemDirectoryA)

__winfnc DWORD GetCurrentDirectoryA(DWORD buf_size, char *buf) {
    const char *dir = "C:\\";
    DWORD len = strlen(dir);
    if(buf && buf_size > len) {
        strcpy(buf, dir);
        return len;
    }
    return len + 1;
}
WINAPI(GetCurrentDirectoryA)