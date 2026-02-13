/*
 * File operation stubs for KERNEL32.dll
 *
 * CreateFileA/W, DeleteFileW, FindFirstFile, FindNextFile, FindClose,
 * FlushFileBuffers, GetFileSizeEx, GetFileType, SetEndOfFile,
 * SetFilePointerEx, CreateDirectoryA, MoveFileExW
 */

#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <poll.h>
#include <errno.h>
#include <sys/ioctl.h>
#include "internal.h"

/* HIDIOCGFEATURE(len) = _IOC(_IOC_WRITE|_IOC_READ, 'H', 0x07, len) */
#define HIDIOCGFEATURE_20 0xC0144807

bool win_hid_pass_heartbeats = false;
volatile bool tudor_shutting_down = false;

/*
 * Image channel monitoring thread.
 * Monitors the image channel fd for reports when active.
 * Exits cleanly when tudor_shutting_down is set or the fd is closed.
 */
static void *img_monitor_thread(void *arg) {
    int fd = *(int *)arg;
    free(arg);
    log_info("img_monitor_thread: started, monitoring image channel fd=%d", fd);

    uint8_t buf[128];
    struct pollfd pfd = { .fd = fd, .events = POLLIN };

    while(!tudor_shutting_down) {
        int ret = poll(&pfd, 1, 500);
        if(tudor_shutting_down) break;
        if(ret < 0) {
            /* fd closed or error — exit */
            log_info("img_monitor_thread: poll error, exiting");
            break;
        }
        if(ret > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            log_info("img_monitor_thread: fd closed/error, exiting");
            break;
        }
        if(ret > 0 && (pfd.revents & POLLIN)) {
            ssize_t n = read(fd, buf, sizeof(buf));
            if(n <= 0) {
                log_info("img_monitor_thread: read returned %zd, exiting", n);
                break;
            }
            char hexbuf[256];
            int hlen = 0;
            for(int i = 0; i < n && i < 40; i++)
                hlen += snprintf(hexbuf + hlen, sizeof(hexbuf) - hlen, "%02x ", buf[i]);
            log_info("img_monitor_thread: IMAGE CHANNEL REPORT! %zd bytes report_id=0x%02x [%s]", n, buf[0], hexbuf);
        }
    }
    log_info("img_monitor_thread: exited");
    return NULL;
}

static bool img_monitor_started = false;

void start_img_monitor(int fd) {
    if(img_monitor_started || fd < 0) return;
    img_monitor_started = true;
    int *fd_arg = malloc(sizeof(int));
    *fd_arg = fd;
    pthread_t thread;
    if(pthread_create(&thread, NULL, img_monitor_thread, fd_arg) == 0) {
        pthread_detach(thread);
    } else {
        free(fd_arg);
    }
}

#define GENERIC_READ 0x80000000
#define GENERIC_WRITE 0x40000000
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define INVALID_FILE_SIZE 0xFFFFFFFF
#define FILE_TYPE_UNKNOWN 0
#define FILE_BEGIN 0
#define FILE_CURRENT 1
#define FILE_END 2

typedef struct {
    DWORD dwFileAttributes;
    DWORD ftCreationTime[2];
    DWORD ftLastAccessTime[2];
    DWORD ftLastWriteTime[2];
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
    DWORD dwReserved0;
    DWORD dwReserved1;
    WCHAR cFileName[260];
    WCHAR cAlternateFileName[14];
} WIN32_FIND_DATAW;

typedef struct {
    DWORD dwFileAttributes;
    DWORD ftCreationTime[2];
    DWORD ftLastAccessTime[2];
    DWORD ftLastWriteTime[2];
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
    DWORD dwReserved0;
    DWORD dwReserved1;
    CHAR cFileName[260];
    CHAR cAlternateFileName[14];
} WIN32_FIND_DATAA;

/*
 * HID device file handle implementation.
 * The DLL opens HID devices via CreateFileA with FILE_FLAG_OVERLAPPED,
 * then does async ReadFile (input reports) and WriteFile (output reports).
 * We route these to the global hidraw fd.
 */

struct hid_read_args {
    int fd;
    OVERLAPPED *ovlp;
    void *buf;
    size_t buf_size;
};

static void *hid_read_thread(void *arg) {
    struct hid_read_args *a = (struct hid_read_args *)arg;
    log_debug("hid_read_thread: reading fd=%d buf_size=%zu", a->fd, a->buf_size);

    ssize_t n;
    for(;;) {
        n = read(a->fd, a->buf, a->buf_size);
        if(n < 0) {
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                /* fd is non-blocking; wait for data with bounded poll */
                struct pollfd pfd = { .fd = a->fd, .events = POLLIN };
                int pret = poll(&pfd, 1, 500);
                if(tudor_shutting_down) { n = -1; errno = ECANCELED; break; }
                if(pret < 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                    break; /* fd closed or error */
                }
                continue;
            }
            break; /* real error (EBADF, etc.) */
        }
        /*
         * Skip synaptic heartbeat ack reports (types 0x01 and 0x02 only).
         * On Linux hidraw, SetFeature responses arrive as input reports
         * (report 0x10). The DLL doesn't expect idle heartbeats — on
         * Windows they go through the control pipe.
         * Pattern: 20 bytes, report_id=0x10, "synaptic" header, total_len=7.
         *
         * IMPORTANT: Only skip type 0x01 (idle A) and 0x02 (idle B).
         * Other types (0x03, 0x04, etc.) may be event notifications
         * (e.g., finger detected) and MUST be passed to the DLL.
         */
        uint8_t *data = (uint8_t*)a->buf;

        /*
         * On BT, hidraw0 delivers ALL non-multitouch reports:
         * keyboard (0x01), consumer (0x02-0x0D), AND fingerprint (0x0F/0x10).
         * Only fingerprint report IDs should reach the DLL.
         */
        if(n > 0 && data[0] != 0x0F && data[0] != 0x10) {
            static int non_fp_skip = 0;
            non_fp_skip++;
            if(non_fp_skip <= 3 || (non_fp_skip % 50) == 0) {
                log_info("hid_read_thread: skipping non-FP report id=0x%02x len=%zd (skip#%d)", data[0], n, non_fp_skip);
            }
            continue;
        }

        if(n == 20 && data[0] == 0x10 &&
           memcmp(data + 1, "synaptic", 8) == 0 &&
           data[9] == 0x07 && data[10] == 0x00 && data[11] == 0x00 && data[12] == 0x00) {
            uint8_t hb_type = data[13];
            /*
             * vfmCaptureProcess interprets: 1=idle/no-finger, 2=FD_DETECTED, 4=RESTART.
             *
             * When win_hid_pass_heartbeats is true (after "wait event"), pass ALL
             * event types to the DLL. The DLL needs both:
             *   - type 0x02 for "finger detected" (finger down)
             *   - type 0x01 for "idle/no finger" (finger removed)
             * Each enrollment touch requires multiple wait/end cycles
             * (detect down, detect up, confirm).
             *
             * When false (pre-capture), filter types 0x01 and 0x02 as heartbeats
             * since they'd confuse the DLL's ReadFile loop.
             */
            /*
             * Pre-capture filter: when pass_heartbeats is false (before
             * "wait event" or after "end event"), filter types 0x01/0x02
             * as idle heartbeats. This prevents stale heartbeats from
             * reaching the DLL during TLS communication where it expects
             * 0x0F data reports.
             */
            if(!win_hid_pass_heartbeats && (hb_type == 0x01 || hb_type == 0x02)) {
                static int hb_skip_count = 0;
                hb_skip_count++;
                if(hb_skip_count <= 3 || (hb_skip_count % 20) == 0) {
                    log_info("hid_read_thread: skipping heartbeat type=0x%02x seq=0x%02x (skip#%d, capture inactive)",
                             hb_type, data[19], hb_skip_count);
                }
                continue;
            }
            if(win_hid_pass_heartbeats) {
                log_info("hid_read_thread: event type=0x%02x seq=0x%02x source=0x%02x → DLL",
                         hb_type, data[19], data[18]);
            }
        }
        break; /* got a real report */
    }

    /*
     * Windows HID ReadFile always returns InputReportByteLength bytes
     * (64 for our device), zero-padding shorter reports (e.g. report
     * 0x10 is only 20 bytes). The DLL checks bytes_transferred and
     * rejects reads that don't match the expected size.
     */
    if(n > 0 && (size_t)n < a->buf_size) {
        memset((uint8_t*)a->buf + n, 0, a->buf_size - n);
        n = (ssize_t)a->buf_size;
    }

    if(n > 0) {
        /* Hex dump first 20 bytes of the report */
        char hexbuf[80];
        int hlen = 0;
        for(int i = 0; i < n && i < 20; i++)
            hlen += snprintf(hexbuf + hlen, sizeof(hexbuf) - hlen, "%02x ", ((uint8_t*)a->buf)[i]);
        log_info("hid_read_thread: read %zd bytes [%s]", n, hexbuf);
    } else {
        log_debug("hid_read_thread: read returned %zd", n);
    }

    if(n > 0) {
        winio_complete_overlapped(a->ovlp, STATUS_SUCCESS, (size_t)n);
    } else if(n == 0) {
        winio_complete_overlapped(a->ovlp, STATUS_CANCELLED, 0);
    } else {
        log_warn("hid_read_thread: read error: %s", strerror(errno));
        winio_complete_overlapped(a->ovlp, STATUS_CANCELLED, 0);
    }

    free(a);
    return NULL;
}

static NTSTATUS hid_file_read(void *ctx, OVERLAPPED *ovlp, off_t offset, void *buf, size_t buf_size, void **op_ctx) {
    int fd = *(int *)ctx;
    log_debug("hid_file_read: starting async read on fd=%d size=%zu", fd, buf_size);

    struct hid_read_args *a = (struct hid_read_args *)malloc(sizeof(struct hid_read_args));
    if(!a) return STATUS_CANCELLED;
    a->fd = fd;
    a->ovlp = ovlp;
    a->buf = buf;
    a->buf_size = buf_size;

    pthread_t thread;
    if(pthread_create(&thread, NULL, hid_read_thread, a) != 0) {
        log_error("hid_file_read: pthread_create failed");
        free(a);
        return STATUS_CANCELLED;
    }
    pthread_detach(thread);

    return STATUS_SUCCESS;
}

static NTSTATUS hid_file_write(void *ctx, OVERLAPPED *ovlp, off_t offset, const void *buf, size_t buf_size, void **op_ctx) {
    int fd = *(int *)ctx;

    /* Hex dump first 20 bytes of what we're sending */
    char hexbuf[80];
    int hlen = 0;
    for(size_t i = 0; i < buf_size && i < 20; i++)
        hlen += snprintf(hexbuf + hlen, sizeof(hexbuf) - hlen, "%02x ", ((const uint8_t*)buf)[i]);
    log_info("hid_file_write: fd=%d size=%zu [%s]", fd, buf_size, hexbuf);

    ssize_t n = write(fd, buf, buf_size);
    log_debug("hid_file_write: write returned %zd", n);

    if(n >= 0) {
        winio_complete_overlapped(ovlp, STATUS_SUCCESS, (size_t)n);
    } else {
        log_warn("hid_file_write: write error: %s", strerror(errno));
        winio_complete_overlapped(ovlp, STATUS_CANCELLED, 0);
    }

    return STATUS_SUCCESS;
}

static void hid_file_destroy(void *ctx) {
    /* Don't close the fd - it's the global hidraw fd managed elsewhere */
    free(ctx);
}

static HANDLE create_hid_file_handle(int fd) {
    if(fd < 0) {
        log_warn("create_hid_file_handle: no hidraw fd available");
        return winhandle_create(NULL, NULL);
    }

    int *fd_ctx = (int *)malloc(sizeof(int));
    if(!fd_ctx) { winerr_set_errno(); return NULL; }
    *fd_ctx = fd;

    log_info("create_hid_file_handle: creating async file handle for hidraw fd=%d", fd);
    return winio_create_file(fd_ctx, true, hid_file_read, hid_file_write, NULL, NULL, NULL, hid_file_destroy);
}

#define FILE_FLAG_OVERLAPPED 0x40000000

/* Determine interface type from device path and return the correct fd */
static int hid_fd_for_path(const char *path) {
    if(path && strstr(path, "MI_01")) {
        log_info("CreateFile: path matches IMAGE channel (MI_01), fd=%d", win_hidraw_fd_img);
        return win_hidraw_fd_img;
    }
    log_info("CreateFile: path matches COMMAND channel, fd=%d", win_hidraw_fd);
    return win_hidraw_fd;
}

static int hid_iface_for_path(const char *path) {
    if(path && strstr(path, "MI_01")) return HID_IFACE_IMG;
    return HID_IFACE_CMD;
}

__winfnc HANDLE CreateFileA(const char *name, DWORD access, DWORD share, void *security, DWORD disposition, DWORD flags, HANDLE tmpl) {
    log_info("CreateFileA called: '%s' access=0x%x disp=%u flags=0x%x", name ? name : "(null)", access, disposition, flags);
    if(disposition == OPEN_EXISTING && (access & (GENERIC_READ | GENERIC_WRITE))) {
        int fd = hid_fd_for_path(name);
        int iface = hid_iface_for_path(name);
        HANDLE h = create_hid_file_handle(fd);
        if(h && h != INVALID_HANDLE_VALUE)
            hid_register_handle(h, iface);
        return h;
    }
    winerr_set();
    return INVALID_HANDLE_VALUE;
}
WINAPI(CreateFileA)

__winfnc HANDLE CreateFileW(const char16_t *name, DWORD access, DWORD share, void *security, DWORD disposition, DWORD flags, HANDLE tmpl) {
    char *cname = winstr_to_str(name);
    log_info("CreateFileW called: '%s' access=0x%x disp=%u flags=0x%x", cname ? cname : "(null)", access, disposition, flags);
    if(disposition == OPEN_EXISTING && (access & (GENERIC_READ | GENERIC_WRITE))) {
        int fd = hid_fd_for_path(cname);
        int iface = hid_iface_for_path(cname);
        free(cname);
        HANDLE h = create_hid_file_handle(fd);
        if(h && h != INVALID_HANDLE_VALUE)
            hid_register_handle(h, iface);
        return h;
    }
    free(cname);
    winerr_set();
    return INVALID_HANDLE_VALUE;
}
WINAPI(CreateFileW)

__winfnc BOOL DeleteFileW(const char16_t *name) {
    log_debug("DeleteFileW called");
    return TRUE;
}
WINAPI(DeleteFileW)

__winfnc HANDLE FindFirstFileA(const char *name, WIN32_FIND_DATAA *data) {
    log_debug("FindFirstFileA called: '%s'", name ? name : "(null)");
    winerr_set();
    return INVALID_HANDLE_VALUE;
}
WINAPI(FindFirstFileA)

#define FINDEX_INFO_STANDARD 0
#define FINDEX_SEARCH_NAME_MATCH 0

__winfnc HANDLE FindFirstFileExW(const char16_t *name, int info_level, WIN32_FIND_DATAW *data, int search_op, void *filter, DWORD flags) {
    log_debug("FindFirstFileExW called");
    winerr_set();
    return INVALID_HANDLE_VALUE;
}
WINAPI(FindFirstFileExW)

__winfnc BOOL FindNextFileA(HANDLE find, WIN32_FIND_DATAA *data) {
    return FALSE;
}
WINAPI(FindNextFileA)

__winfnc BOOL FindNextFileW(HANDLE find, WIN32_FIND_DATAW *data) {
    return FALSE;
}
WINAPI(FindNextFileW)

__winfnc BOOL FindClose(HANDLE find) {
    return TRUE;
}
WINAPI(FindClose)

__winfnc BOOL FlushFileBuffers(HANDLE file) {
    return TRUE;
}
WINAPI(FlushFileBuffers)

typedef struct {
    LONG LowPart;
    LONG HighPart;
} LARGE_INTEGER;

__winfnc BOOL GetFileSizeEx(HANDLE file, LARGE_INTEGER *size) {
    if(size) { size->LowPart = 0; size->HighPart = 0; }
    return FALSE;
}
WINAPI(GetFileSizeEx)

__winfnc DWORD GetFileType(HANDLE file) {
    return FILE_TYPE_UNKNOWN;
}
WINAPI(GetFileType)

__winfnc BOOL SetEndOfFile(HANDLE file) {
    return TRUE;
}
WINAPI(SetEndOfFile)

__winfnc BOOL SetFilePointerEx(HANDLE file, LONGLONG distance, LARGE_INTEGER *new_pos, DWORD method) {
    if(new_pos) { new_pos->LowPart = 0; new_pos->HighPart = 0; }
    return TRUE;
}
WINAPI(SetFilePointerEx)

__winfnc BOOL CreateDirectoryA(const char *path, void *security) {
    log_debug("CreateDirectoryA called: '%s'", path ? path : "(null)");
    return TRUE;
}
WINAPI(CreateDirectoryA)

__winfnc BOOL MoveFileExW(const char16_t *old_name, const char16_t *new_name, DWORD flags) {
    log_debug("MoveFileExW called");
    return TRUE;
}
WINAPI(MoveFileExW)
