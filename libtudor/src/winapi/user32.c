/*
 * USER32.dll function stubs for synaWudfBioHid153.dll
 *
 * The DLL creates a hidden message window for power setting notifications.
 * We stub all window/message functions as no-ops since we handle power
 * management differently on Linux.
 */

#include <string.h>
#include <unistd.h>
#include "internal.h"

/* Window class / message types */
typedef struct {
    UINT cbSize;
    UINT style;
    void *lpfnWndProc;
    int cbClsExtra;
    int cbWndExtra;
    HANDLE hInstance;
    HANDLE hIcon;
    HANDLE hCursor;
    HANDLE hbrBackground;
    const char16_t *lpszMenuName;
    const char16_t *lpszClassName;
    HANDLE hIconSm;
} WNDCLASSEXW;

typedef struct {
    HANDLE hwnd;
    UINT message;
    ULONG_PTR wParam;
    LONG_PTR lParam;
    DWORD time;
    POINT pt;
} MSG;

typedef struct {
    HANDLE hdc;
    BOOL fErase;
    RECT rcPaint;
    BOOL fRestore;
    BOOL fIncUpdate;
    BYTE rgbReserved[32];
} PAINTSTRUCT;

/* Fake window handle */
#define FAKE_HWND ((HANDLE)(uintptr_t)0x0000CAFE)
/* Fake atom for RegisterClassExW */
#define FAKE_ATOM 0xBEEF
/* Fake power notification handle */
#define FAKE_POWER_HANDLE ((HANDLE)(uintptr_t)0x0000D00D)

__winfnc USHORT RegisterClassExW(const WNDCLASSEXW *wndclass) {
    log_debug("RegisterClassExW called (class=%p)", wndclass);
    return FAKE_ATOM;
}
WINAPI(RegisterClassExW)

__winfnc BOOL UnregisterClassW(const char16_t *class_name, HANDLE hInstance) {
    log_debug("UnregisterClassW called");
    return TRUE;
}
WINAPI(UnregisterClassW)

__winfnc HANDLE CreateWindowExW(DWORD ex_style, const char16_t *class_name, const char16_t *window_name,
                                DWORD style, int x, int y, int width, int height,
                                HANDLE parent, HANDLE menu, HANDLE instance, void *param) {
    log_debug("CreateWindowExW called");
    return FAKE_HWND;
}
WINAPI(CreateWindowExW)

__winfnc BOOL DestroyWindow(HANDLE hwnd) {
    log_debug("DestroyWindow called");
    return TRUE;
}
WINAPI(DestroyWindow)

__winfnc LONG_PTR DefWindowProcW(HANDLE hwnd, UINT msg, ULONG_PTR wParam, LONG_PTR lParam) {
    return 0;
}
WINAPI(DefWindowProcW)

__winfnc LONG_PTR CallWindowProcW(void *prev_wnd_func, HANDLE hwnd, UINT msg, ULONG_PTR wParam, LONG_PTR lParam) {
    return 0;
}
WINAPI(CallWindowProcW)

__winfnc BOOL GetMessageW(MSG *msg, HANDLE hwnd, UINT msg_filter_min, UINT msg_filter_max) {
    log_debug("GetMessageW called - blocking forever (no message loop)");
    /* Block forever - this is the message pump thread and we don't have messages.
     * The DLL creates a thread that calls GetMessageW in a loop. We just sleep. */
    while (1) {
        usleep(60000000); /* 60 seconds */
    }
    return FALSE;
}
WINAPI(GetMessageW)

__winfnc BOOL TranslateMessage(const MSG *msg) {
    return FALSE;
}
WINAPI(TranslateMessage)

__winfnc LONG_PTR DispatchMessageW(const MSG *msg) {
    return 0;
}
WINAPI(DispatchMessageW)

__winfnc BOOL PostThreadMessageW(DWORD thread_id, UINT msg, ULONG_PTR wParam, LONG_PTR lParam) {
    log_debug("PostThreadMessageW called (thread=%u, msg=0x%x)", thread_id, msg);
    return TRUE;
}
WINAPI(PostThreadMessageW)

__winfnc LONG_PTR GetWindowLongPtrW(HANDLE hwnd, int index) {
    return 0;
}
WINAPI(GetWindowLongPtrW)

__winfnc LONG_PTR SetWindowLongPtrW(HANDLE hwnd, int index, LONG_PTR new_long) {
    return 0;
}
WINAPI(SetWindowLongPtrW)

__winfnc HANDLE RegisterPowerSettingNotification(HANDLE recipient, const GUID *setting, DWORD flags) {
    log_debug("RegisterPowerSettingNotification called");
    return FAKE_POWER_HANDLE;
}
WINAPI(RegisterPowerSettingNotification)

__winfnc BOOL UnregisterPowerSettingNotification(HANDLE handle) {
    log_debug("UnregisterPowerSettingNotification called");
    return TRUE;
}
WINAPI(UnregisterPowerSettingNotification)

__winfnc HANDLE BeginPaint(HANDLE hwnd, PAINTSTRUCT *ps) {
    if (ps) memset(ps, 0, sizeof(PAINTSTRUCT));
    return NULL;
}
WINAPI(BeginPaint)

__winfnc BOOL EndPaint(HANDLE hwnd, const PAINTSTRUCT *ps) {
    return TRUE;
}
WINAPI(EndPaint)
