#ifndef LIBTUDOR_WINAPI_INTERNAL_H
#define LIBTUDOR_WINAPI_INTERNAL_H

#include "api.h"

//Threading
void win_init_tib();
DWORD win_get_thread_id();

//Synchronization
struct win_sync_object;

#define INFINITE 0xffffffff
#define WAIT_TIMEOUT 0x00000102L

typedef DWORD win_sync_obj_wait_fnc(struct win_sync_object *sync_obj, DWORD timeout);

struct win_sync_object {
    win_sync_obj_wait_fnc *wait_fnc;
};

DWORD win_wait_sync_obj(HANDLE handle, DWORD timeout);

HANDLE win_create_event(const char *name, bool initial_state, bool manual_reset);
void win_set_event(HANDLE evt);
void win_reset_event(HANDLE evt);

//HID device fds for HidD_* functions
extern int win_hidraw_fd;       // command channel (reports 0x0E/0x0F/0x10/0x11)
extern int win_hidraw_fd_img;   // image channel (reports 0x0C/0x0D/0x20/0x21), -1 if unavailable

//When true, events (report 0x10) are passed through to the DLL.
//Set after "wait event", cleared after "end event".
extern bool win_hid_pass_heartbeats;

//Set to true during shutdown to signal background threads to exit
extern volatile bool tudor_shutting_down;


//Start monitoring the image channel for any reports (finger events, etc.)
void start_img_monitor(int fd);

//HID interface types
#define HID_IFACE_CMD 0
#define HID_IFACE_IMG 1

//Handle-to-interface tracking (set by CreateFileA/W, used by HidD_*)
void hid_register_handle(void *handle, int iface_type);
int hid_get_handle_type(void *handle);
int hid_get_fd_for_handle(void *handle);

#endif