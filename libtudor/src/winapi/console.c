/*
 * Console I/O stubs for KERNEL32.dll
 *
 * The DLL's CRT startup code references these but they're never
 * meaningfully used in a driver context.
 */

#include "internal.h"

__winfnc UINT GetConsoleCP() {
    return 65001; /* UTF-8 */
}
WINAPI(GetConsoleCP)

__winfnc UINT GetConsoleOutputCP() {
    return 65001;
}
WINAPI(GetConsoleOutputCP)

__winfnc BOOL GetConsoleMode(HANDLE console, DWORD *mode) {
    if(mode) *mode = 0;
    return FALSE;
}
WINAPI(GetConsoleMode)

__winfnc BOOL SetConsoleMode(HANDLE console, DWORD mode) {
    return FALSE;
}
WINAPI(SetConsoleMode)

__winfnc BOOL GetNumberOfConsoleInputEvents(HANDLE console, DWORD *num_events) {
    if(num_events) *num_events = 0;
    return FALSE;
}
WINAPI(GetNumberOfConsoleInputEvents)

__winfnc BOOL PeekConsoleInputA(HANDLE console, void *buf, DWORD len, DWORD *num_events_read) {
    if(num_events_read) *num_events_read = 0;
    return FALSE;
}
WINAPI(PeekConsoleInputA)

__winfnc BOOL ReadConsoleInputW(HANDLE console, void *buf, DWORD len, DWORD *num_events_read) {
    if(num_events_read) *num_events_read = 0;
    return FALSE;
}
WINAPI(ReadConsoleInputW)

__winfnc BOOL ReadConsoleW(HANDLE console, void *buf, DWORD chars_to_read, DWORD *chars_read, void *input_ctrl) {
    if(chars_read) *chars_read = 0;
    return FALSE;
}
WINAPI(ReadConsoleW)

__winfnc BOOL WriteConsoleW(HANDLE console, const void *buf, DWORD chars_to_write, DWORD *chars_written, void *reserved) {
    if(chars_written) *chars_written = chars_to_write;
    return TRUE;
}
WINAPI(WriteConsoleW)
