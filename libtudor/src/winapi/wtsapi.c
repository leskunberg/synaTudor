/*
 * WTSAPI32.dll function stubs for synaWudfBioHid153.dll
 *
 * Terminal Services session notification functions. The DLL uses these
 * to detect session lock/unlock. Stubbed as no-ops.
 */

#include <string.h>
#include "internal.h"

#define WTS_CURRENT_SESSION ((DWORD)-1)

__winfnc BOOL WTSRegisterSessionNotification(HANDLE hwnd, DWORD flags) {
    log_debug("WTSRegisterSessionNotification called");
    return TRUE;
}
WINAPI(WTSRegisterSessionNotification)

__winfnc BOOL WTSUnRegisterSessionNotification(HANDLE hwnd) {
    log_debug("WTSUnRegisterSessionNotification called");
    return TRUE;
}
WINAPI(WTSUnRegisterSessionNotification)

__winfnc BOOL WTSQuerySessionInformationW(HANDLE server, DWORD session_id, int wts_info_class, char16_t **buf, DWORD *bytes_returned) {
    log_debug("WTSQuerySessionInformationW called (class=%d)", wts_info_class);
    /* Return empty string for any query */
    if (buf) {
        *buf = (char16_t *)malloc(sizeof(char16_t));
        if (*buf) (*buf)[0] = u'\0';
    }
    if (bytes_returned) *bytes_returned = sizeof(char16_t);
    return TRUE;
}
WINAPI(WTSQuerySessionInformationW)

__winfnc void WTSFreeMemory(void *mem) {
    free(mem);
}
WINAPI(WTSFreeMemory)
