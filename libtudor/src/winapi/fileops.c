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
 *
 * A single persistent reader thread per fd handles all async reads.
 * Read requests are queued and the reader thread dispatches reports to
 * the oldest pending request. This avoids spawning unbounded threads
 * (on BT, many non-FP reports are filtered in a loop, so thread-per-read
 * causes threads to accumulate past sandbox limits).
 */

struct hid_read_req {
    OVERLAPPED *ovlp;
    void *buf;
    size_t buf_size;
    struct hid_read_req *next;
};

struct hid_reader_ctx {
    int fd;
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool thread_started;
    bool shutdown;
    struct hid_read_req *queue_head;
    struct hid_read_req *queue_tail;
};

/*
 * Single persistent reader thread. Waits for queued read requests,
 * reads from hidraw, filters non-FP/heartbeat reports, and completes
 * the oldest pending request when a valid report arrives.
 */
static void *hid_reader_thread(void *arg) {
    struct hid_reader_ctx *rctx = (struct hid_reader_ctx *)arg;
    log_info("hid_reader_thread: started for fd=%d", rctx->fd);

    while(!tudor_shutting_down && !rctx->shutdown) {
        /* Wait for a pending read request */
        pthread_mutex_lock(&rctx->lock);
        while(!rctx->queue_head && !tudor_shutting_down && !rctx->shutdown) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;
            pthread_cond_timedwait(&rctx->cond, &rctx->lock, &ts);
        }
        if(tudor_shutting_down || rctx->shutdown) {
            /* Cancel all pending reads */
            struct hid_read_req *r = rctx->queue_head;
            rctx->queue_head = rctx->queue_tail = NULL;
            pthread_mutex_unlock(&rctx->lock);
            while(r) {
                struct hid_read_req *next = r->next;
                winio_complete_overlapped(r->ovlp, STATUS_CANCELLED, 0);
                free(r);
                r = next;
            }
            break;
        }
        struct hid_read_req *req = rctx->queue_head;
        rctx->queue_head = req->next;
        if(!rctx->queue_head) rctx->queue_tail = NULL;
        pthread_mutex_unlock(&rctx->lock);

        /* Read loop with filtering (same logic as before) */
        ssize_t n;
        for(;;) {
            n = read(rctx->fd, req->buf, req->buf_size);
            if(n < 0) {
                if(errno == EAGAIN || errno == EWOULDBLOCK) {
                    struct pollfd pfd = { .fd = rctx->fd, .events = POLLIN };
                    int pret = poll(&pfd, 1, 500);
                    if(tudor_shutting_down || rctx->shutdown) { n = -1; errno = ECANCELED; break; }
                    if(pret < 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                        break;
                    }
                    continue;
                }
                break;
            }

            uint8_t *data = (uint8_t*)req->buf;

            /* On BT, filter non-FP reports (keyboard, consumer, etc.) */
            if(n > 0 && data[0] != 0x0F && data[0] != 0x10) {
                static int non_fp_skip = 0;
                non_fp_skip++;
                if(non_fp_skip <= 3 || (non_fp_skip % 50) == 0) {
                    log_info("hid_reader: skipping non-FP report id=0x%02x len=%zd (skip#%d)", data[0], n, non_fp_skip);
                }
                continue;
            }

            /* Filter synaptic heartbeats when not in capture mode */
            if(n == 20 && data[0] == 0x10 &&
               memcmp(data + 1, "synaptic", 8) == 0 &&
               data[9] == 0x07 && data[10] == 0x00 && data[11] == 0x00 && data[12] == 0x00) {
                uint8_t hb_type = data[13];
                if(!win_hid_pass_heartbeats && (hb_type == 0x01 || hb_type == 0x02)) {
                    static int hb_skip_count = 0;
                    hb_skip_count++;
                    if(hb_skip_count <= 3 || (hb_skip_count % 20) == 0) {
                        log_info("hid_reader: skipping heartbeat type=0x%02x seq=0x%02x (skip#%d, capture inactive)",
                                 hb_type, data[19], hb_skip_count);
                    }
                    continue;
                }
                if(win_hid_pass_heartbeats) {
                    log_info("hid_reader: event type=0x%02x seq=0x%02x source=0x%02x → DLL",
                             hb_type, data[19], data[18]);
                }
            }
            break; /* got a real report */
        }

        /* Zero-pad to match Windows HID InputReportByteLength behavior */
        if(n > 0 && (size_t)n < req->buf_size) {
            memset((uint8_t*)req->buf + n, 0, req->buf_size - n);
            n = (ssize_t)req->buf_size;
        }

        if(n > 0) {
            char hexbuf[80];
            int hlen = 0;
            for(int i = 0; i < n && i < 20; i++)
                hlen += snprintf(hexbuf + hlen, sizeof(hexbuf) - hlen, "%02x ", ((uint8_t*)req->buf)[i]);
            log_info("hid_reader: read %zd bytes [%s]", n, hexbuf);
        }

        if(n > 0) {
            winio_complete_overlapped(req->ovlp, STATUS_SUCCESS, (size_t)n);
        } else if(n == 0) {
            winio_complete_overlapped(req->ovlp, STATUS_CANCELLED, 0);
        } else {
            log_warn("hid_reader: read error: %s", strerror(errno));
            winio_complete_overlapped(req->ovlp, STATUS_CANCELLED, 0);
        }
        free(req);
    }

    log_info("hid_reader_thread: exiting for fd=%d", rctx->fd);
    return NULL;
}

static NTSTATUS hid_file_read(void *ctx, OVERLAPPED *ovlp, off_t offset, void *buf, size_t buf_size, void **op_ctx) {
    struct hid_reader_ctx *rctx = (struct hid_reader_ctx *)ctx;
    log_debug("hid_file_read: queuing async read on fd=%d size=%zu", rctx->fd, buf_size);

    struct hid_read_req *req = (struct hid_read_req *)malloc(sizeof(struct hid_read_req));
    if(!req) return STATUS_CANCELLED;
    req->ovlp = ovlp;
    req->buf = buf;
    req->buf_size = buf_size;
    req->next = NULL;

    pthread_mutex_lock(&rctx->lock);

    /* Start reader thread on first read */
    if(!rctx->thread_started) {
        if(pthread_create(&rctx->thread, NULL, hid_reader_thread, rctx) != 0) {
            log_error("hid_file_read: pthread_create failed: %s", strerror(errno));
            pthread_mutex_unlock(&rctx->lock);
            free(req);
            return STATUS_CANCELLED;
        }
        rctx->thread_started = true;
    }

    /* Enqueue */
    if(rctx->queue_tail) rctx->queue_tail->next = req;
    else rctx->queue_head = req;
    rctx->queue_tail = req;
    pthread_cond_signal(&rctx->cond);

    pthread_mutex_unlock(&rctx->lock);
    return STATUS_SUCCESS;
}

static NTSTATUS hid_file_write(void *ctx, OVERLAPPED *ovlp, off_t offset, const void *buf, size_t buf_size, void **op_ctx) {
    struct hid_reader_ctx *rctx = (struct hid_reader_ctx *)ctx;
    int fd = rctx->fd;

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
    struct hid_reader_ctx *rctx = (struct hid_reader_ctx *)ctx;
    /* Signal reader thread to stop and wait for it */
    pthread_mutex_lock(&rctx->lock);
    rctx->shutdown = true;
    pthread_cond_signal(&rctx->cond);
    pthread_mutex_unlock(&rctx->lock);
    if(rctx->thread_started) {
        pthread_join(rctx->thread, NULL);
    }
    pthread_mutex_destroy(&rctx->lock);
    pthread_cond_destroy(&rctx->cond);
    /* Don't close the fd - it's the global hidraw fd managed elsewhere */
    free(rctx);
}

static HANDLE create_hid_file_handle(int fd) {
    if(fd < 0) {
        log_warn("create_hid_file_handle: no hidraw fd available");
        return winhandle_create(NULL, NULL);
    }

    struct hid_reader_ctx *rctx = (struct hid_reader_ctx *)calloc(1, sizeof(struct hid_reader_ctx));
    if(!rctx) { winerr_set_errno(); return NULL; }
    rctx->fd = fd;
    pthread_mutex_init(&rctx->lock, NULL);
    pthread_cond_init(&rctx->cond, NULL);

    log_info("create_hid_file_handle: creating async file handle for hidraw fd=%d", fd);
    return winio_create_file(rctx, true, hid_file_read, hid_file_write, NULL, NULL, NULL, hid_file_destroy);
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
