#include "internal.h"
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <linux/hidraw.h>

/*
 * HID transport layer for Tudor fingerprint sensors connected via HID
 * (e.g., embedded in Lenovo ThinkPad keyboards via Bluetooth/USB HID).
 *
 * Replaces usb.c which uses libusb for raw USB bulk transfers.
 * This implementation uses Linux hidraw to send/receive HID reports:
 *   - Output Report 0x0E (20 bytes): sensor commands (fragmented)
 *   - Input Report 0x0F (64 bytes): sensor responses (fragmented)
 *   - Feature Report 0x11 (20 bytes): init/control
 *
 * The "synaptic" framing protocol is the same - we just handle the
 * HID report fragmentation/reassembly layer underneath.
 */

#define HID_REPORT_OUT_ID     0x0E
#define HID_REPORT_IN_ID      0x0F
#define HID_REPORT_STATUS_ID  0x10
#define HID_REPORT_FEATURE_ID 0x11

#define HID_REPORT_OUT_SIZE    20  /* 1 report_id + 19 data */
#define HID_REPORT_IN_SIZE     64  /* 1 report_id + 63 data */
#define HID_REPORT_FEATURE_SIZE 20 /* 1 report_id + 19 data */

#define HID_OUT_DATA_SIZE   19  /* payload per output report */
#define HID_IN_DATA_SIZE    63  /* payload per input report */

#define SYNAPTIC_MAGIC "synaptic"
#define SYNAPTIC_MAGIC_LEN 8

/* Framing codes for multi-frame synaptic messages */
#define FRAME_SINGLE 0xC0FF
#define FRAME_FIRST  0x4000
#define FRAME_LAST   0x80FF

/* Max payload bytes per synaptic frame (data before framing split) */
#define MAX_FRAG 248

/* Fake USB descriptors for the DLL */
typedef struct {
    ULONG USBDI_Version;
    ULONG Supported_USB_Version;
} USBD_VERSION_INFORMATION;

typedef struct {
    UCHAR bLength;
    UCHAR bDescriptorType;
    USHORT bcdUSB;
    UCHAR bDeviceClass;
    UCHAR bDeviceSubClass;
    UCHAR bDeviceProtocol;
    UCHAR bMaxPacketSize0;
    USHORT idVendor;
    USHORT idProduct;
    USHORT bcdDevice;
    UCHAR iManufacturer;
    UCHAR iProduct;
    UCHAR iSerialNumber;
    UCHAR bNumConfigurations;
} USB_DEVICE_DESCRIPTOR;

typedef struct {
    UCHAR bLength;
    UCHAR bDescriptorType;
    UCHAR bInterfaceNumber;
    UCHAR bAlternateSetting;
    UCHAR bNumEndpoints;
    UCHAR bInterfaceClass;
    UCHAR bInterfaceSubClass;
    UCHAR bInterfaceProtocol;
    UCHAR iInterface;
} USB_INTERFACE_DESCRIPTOR;

typedef struct {
    ULONG Size;
    USBD_VERSION_INFORMATION UsbdVersionInformation;
    ULONG HcdPortCapabilities;
    ULONG Traits;
} WDF_USB_DEVICE_INFORMATION;

typedef enum {
    WdfUsbPipeTypeInvalid,
    WdfUsbPipeTypeControl,
    WdfUsbPipeTypeIsochronous,
    WdfUsbPipeTypeBulk,
    WdfUsbPipeTypeInterrupt
} WDF_USB_PIPE_TYPE;

typedef struct {
    ULONG Size;
    ULONG MaximumPacketSize;
    UCHAR EndpointAddress;
    UCHAR Interval;
    UCHAR SettingIndex;
    WDF_USB_PIPE_TYPE PipeType;
    ULONG MaximumTransferSize;
} WDF_USB_PIPE_INFORMATION;

typedef struct {
    ULONG Size;
    ULONG Flags;
    LONGLONG Timeout;
} WDF_REQUEST_SEND_OPTIONS;

struct wdf_hid_pipe;

struct wdf_hid_device {
    struct wdf_object object;
    struct winwdf_device *device;
    bool is_dying;

    int hidraw_fd;
    pthread_mutex_t io_lock;

    struct wdf_hid_pipe *pipes;
    int num_pipes;
};

struct wdf_hid_pipe {
    struct wdf_object object;
    struct wdf_hid_device *hid_dev;
    bool is_input;  /* true = IN (read from sensor), false = OUT (write to sensor) */
    UCHAR endpoint_addr;
};

/* ========== HID I/O helpers ========== */

bool hid_init_sensor(int fd) {
    log_info("hid_init_sensor: ENTER (fd=%d)", fd);
    uint8_t buf[HID_REPORT_FEATURE_SIZE];

    /* First init: write "synaptic" [02] [0x81 0x01] */
    memset(buf, 0, sizeof(buf));
    buf[0] = HID_REPORT_FEATURE_ID;
    memcpy(buf + 1, SYNAPTIC_MAGIC, SYNAPTIC_MAGIC_LEN);
    buf[9] = 0x02; buf[10] = 0x00; buf[11] = 0x00; buf[12] = 0x00; /* total_len = 2 */
    buf[13] = 0x81; buf[14] = 0x01;

    if (ioctl(fd, HIDIOCSFEATURE(sizeof(buf)), buf) < 0) {
        log_error("Failed to send HID feature init 1: %s", strerror(errno));
        return false;
    }

    usleep(100000); /* 100ms */

    /* Second init: write "synaptic" [01] [0x0F] */
    memset(buf, 0, sizeof(buf));
    buf[0] = HID_REPORT_FEATURE_ID;
    memcpy(buf + 1, SYNAPTIC_MAGIC, SYNAPTIC_MAGIC_LEN);
    buf[9] = 0x01; buf[10] = 0x00; buf[11] = 0x00; buf[12] = 0x00; /* total_len = 1 */
    buf[13] = 0x0F;

    if (ioctl(fd, HIDIOCSFEATURE(sizeof(buf)), buf) < 0) {
        log_error("Failed to send HID feature init 2: %s", strerror(errno));
        return false;
    }

    usleep(300000); /* 300ms */

    /* Drain any pending input */
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    while (poll(&pfd, 1, 50) > 0) {
        uint8_t drain[256];
        if (read(fd, drain, sizeof(drain)) <= 0) break;
    }

    return true;
}

/* Build one synaptic frame and fragment into 0x0E HID output reports.
 * chunk/chunk_len = the data portion for this frame (raw VCSFW or fragment thereof).
 * framing = framing code (0xC0FF single, 0x4000 first, 0x80FF last, or index for middle). */
static int hid_write_one_frame(int fd, const uint8_t *chunk, size_t chunk_len, uint16_t framing) {
    /* Build the synaptic frame header in the first report:
     * [0x0E] "synaptic"(8) total_len(4) framing(2) data(5 max) */
    uint32_t total_len = 2 + (uint32_t)chunk_len;  /* framing(2) + data */

    uint8_t first[HID_REPORT_OUT_SIZE];
    memset(first, 0, sizeof(first));
    first[0] = HID_REPORT_OUT_ID;
    memcpy(first + 1, SYNAPTIC_MAGIC, SYNAPTIC_MAGIC_LEN);
    memcpy(first + 9, &total_len, 4);
    memcpy(first + 13, &framing, 2);

    /* First 5 bytes of data go into the first report */
    size_t first_data = chunk_len < 5 ? chunk_len : 5;
    if (first_data > 0) memcpy(first + 15, chunk, first_data);

    ssize_t written = write(fd, first, sizeof(first));
    if (written < 0) {
        log_error("hidraw write failed: %s", strerror(errno));
        return -1;
    }

    /* Continuation reports for remaining data */
    size_t offset = first_data;
    while (offset < chunk_len) {
        usleep(3000);

        uint8_t report[HID_REPORT_OUT_SIZE];
        memset(report, 0, sizeof(report));
        report[0] = HID_REPORT_OUT_ID;

        size_t n = chunk_len - offset;
        if (n > HID_OUT_DATA_SIZE) n = HID_OUT_DATA_SIZE;
        memcpy(report + 1, chunk + offset, n);
        offset += n;

        written = write(fd, report, sizeof(report));
        if (written < 0) {
            log_error("hidraw write (cont) failed: %s", strerror(errno));
            return -1;
        }
    }

    return 0;
}

/* Send raw VCSFW data wrapped in synaptic frame(s).
 * Handles multi-frame fragmentation for large payloads (> MAX_FRAG bytes). */
static int hid_send_vcsfw(int fd, const uint8_t *vcsfw_data, size_t vcsfw_len) {
    if (vcsfw_len <= MAX_FRAG) {
        /* Single frame */
        return hid_write_one_frame(fd, vcsfw_data, vcsfw_len, FRAME_SINGLE);
    }

    /* Multi-frame: split into MAX_FRAG-sized chunks */
    size_t offset = 0;
    int idx = 0;
    while (offset < vcsfw_len) {
        size_t end = offset + MAX_FRAG;
        if (end > vcsfw_len) end = vcsfw_len;
        size_t chunk_len = end - offset;

        uint16_t framing;
        if (idx == 0) framing = FRAME_FIRST;
        else if (end >= vcsfw_len) framing = FRAME_LAST;
        else framing = (uint16_t)idx;

        int ret = hid_write_one_frame(fd, vcsfw_data + offset, chunk_len, framing);
        if (ret < 0) return ret;

        offset = end;
        idx++;
        usleep(50000);  /* 50ms between frames */
    }

    return 0;
}

/* Read HID input reports and extract the raw VCSFW response (stripping synaptic framing).
 * Response format in first 0x0F report:
 *   [0x0F] "synaptic"(8B) total_len(4B) payload_len(4B) VCSFW_data...
 * Continuation 0x0F reports:
 *   [0x0F] VCSFW_data...
 *
 * Returns number of VCSFW response bytes written to buf, or -1 on error. */
static ssize_t hid_recv_vcsfw(int fd, uint8_t *buf, size_t buf_size, int timeout_ms) {
    size_t total = 0;
    uint32_t payload_len = 0;
    bool got_header = false;

    struct pollfd pfd = { .fd = fd, .events = POLLIN };

    while (1) {
        int ret = poll(&pfd, 1, got_header ? 200 : timeout_ms);
        if (ret < 0) {
            log_error("poll failed: %s", strerror(errno));
            return -1;
        }
        if (ret == 0) {
            /* Timeout */
            if (total > 0) return total;
            log_warn("HID read timeout (%d ms)", timeout_ms);
            return -1;
        }

        uint8_t report[HID_REPORT_IN_SIZE];
        ssize_t n = read(fd, report, sizeof(report));
        if (n <= 1) {
            if (total > 0) return total;
            return -1;
        }

        /* Only process 0x0F and 0x10 reports */
        if (report[0] != HID_REPORT_IN_ID && report[0] != HID_REPORT_STATUS_ID)
            continue;

        if (!got_header && n >= 17 && memcmp(report + 1, SYNAPTIC_MAGIC, SYNAPTIC_MAGIC_LEN) == 0) {
            /* First response report with synaptic header:
             * [0]    = 0x0F report_id
             * [1:9]  = "synaptic"
             * [9:13] = total_len (includes payload_len field + payload)
             * [13:17] = payload_len (VCSFW response size)
             * [17:]  = VCSFW response data (first chunk) */
            got_header = true;
            memcpy(&payload_len, report + 13, 4);

            size_t avail = (size_t)(n - 17);
            size_t copy = avail;
            if (copy > payload_len) copy = payload_len;
            if (copy > buf_size) copy = buf_size;
            if (copy > 0) {
                memcpy(buf, report + 17, copy);
                total = copy;
            }

            if (total >= payload_len) return total;
        } else if (got_header) {
            /* Continuation report: VCSFW data starts at byte 1 */
            size_t avail = (size_t)(n - 1);
            size_t remaining = payload_len - total;
            size_t copy = avail < remaining ? avail : remaining;
            if (copy > buf_size - total) copy = buf_size - total;
            if (copy > 0) {
                memcpy(buf + total, report + 1, copy);
                total += copy;
            }

            if (total >= payload_len) return total;
        }
    }
}

/* ========== WDF USB function implementations ========== */

static void hid_dev_destr(struct wdf_hid_device *hid_dev) {
    wdf_set_usb_device(hid_dev->device, NULL);

    hid_dev->is_dying = true;
    cant_fail_ret(pthread_mutex_destroy(&hid_dev->io_lock));

    /* Destroy pipes */
    if (hid_dev->pipes) {
        for (int i = 0; i < hid_dev->num_pipes; i++) {
            wdf_cleanup_obj(&hid_dev->pipes[i].object);
        }
        free(hid_dev->pipes);
    }

    wdf_cleanup_obj(&hid_dev->object);
    free(hid_dev);
}

__winfnc NTSTATUS WdfUsbTargetDeviceCreate(WDF_DRIVER_GLOBALS *globals, WDFOBJECT dev_obj, WDF_OBJECT_ATTRIBUTES *obj_attrs, WDFOBJECT *out) {
    /*
     * Strategy B: Return failure so the DLL falls back to its native HID
     * code path. The HID 153 DLL is designed for HID devices and should
     * handle finger detection differently when USB isn't available
     * (e.g., polling Feature 0x11 instead of waiting for input reports).
     * The sensor channel has already been initialized in WdfDeviceCreate.
     */
    log_info("WdfUsbTargetDeviceCreate: FAILING intentionally (Strategy B - force HID code path)");
    *out = NULL;
    return WINERR_SET_CODE;
}
WDFFUNC(WdfUsbTargetDeviceCreate, 202)

__winfnc NTSTATUS WdfUsbTargetDeviceRetrieveInformation(WDF_DRIVER_GLOBALS *globals, WDFOBJECT usb_dev_obj, WDF_USB_DEVICE_INFORMATION *ver) {
    log_info("WdfUsbTargetDeviceRetrieveInformation: ENTER (usb_dev=%p)", usb_dev_obj);
    ver->Size = sizeof(WDF_USB_DEVICE_INFORMATION);
    ver->UsbdVersionInformation.USBDI_Version = 0x00000600;
    ver->UsbdVersionInformation.Supported_USB_Version = 0x0200;
    ver->HcdPortCapabilities = 0;
    ver->Traits = 0;
    return STATUS_SUCCESS;
}
WDFFUNC(WdfUsbTargetDeviceRetrieveInformation, 204)

__winfnc NTSTATUS WdfUsbTargetDeviceGetDeviceDescriptor(WDF_DRIVER_GLOBALS *globals, WDFOBJECT usb_dev_obj, USB_DEVICE_DESCRIPTOR *descrp) {
    log_info("WdfUsbTargetDeviceGetDeviceDescriptor: ENTER (usb_dev=%p)", usb_dev_obj);
    /* Return fake USB descriptor matching the sensor */
    *descrp = (USB_DEVICE_DESCRIPTOR) {
        .bLength = sizeof(USB_DEVICE_DESCRIPTOR),
        .bDescriptorType = 1,  /* DEVICE */
        .bcdUSB = 0x0200,
        .bDeviceClass = 0,
        .bDeviceSubClass = 0,
        .bDeviceProtocol = 0,
        .bMaxPacketSize0 = 64,
        .idVendor = 0x06cb,   /* Synaptics */
        .idProduct = 0x00dd,  /* Tudor HID PID (our sensor) */
        .bcdDevice = 0x0100,
        .iManufacturer = 1,
        .iProduct = 2,
        .iSerialNumber = 3,
        .bNumConfigurations = 1
    };
    return STATUS_SUCCESS;
}
WDFFUNC(WdfUsbTargetDeviceGetDeviceDescriptor, 205)

__winfnc NTSTATUS WdfUsbTargetDeviceResetPortSynchronously(WDF_DRIVER_GLOBALS *globals, WDFOBJECT usb_dev_obj) {
    /* No-op for HID - can't reset the port */
    log_info("WdfUsbTargetDeviceResetPortSynchronously: ENTER (no-op for HID)");
    return STATUS_SUCCESS;
}
WDFFUNC(WdfUsbTargetDeviceResetPortSynchronously, 214)

__winfnc WDFOBJECT WdfUsbTargetDeviceGetInterface(WDF_DRIVER_GLOBALS *globals, WDFOBJECT usb_dev_obj, UCHAR interface_idx) {
    log_info("WdfUsbTargetDeviceGetInterface: ENTER (usb_dev=%p, idx=%d)", usb_dev_obj, interface_idx);
    struct wdf_hid_device *hid_dev = (struct wdf_hid_device*) usb_dev_obj;

    if (interface_idx != 0) return NULL;

    /* Return a fake interface object - just reuse the hid_dev object itself
     * since we only have one "interface" */
    /* Actually, we need to return something that the DLL can query for pipes.
     * Create a wrapper that WdfUsbInterfaceGetConfiguredPipe can use. */

    /* We'll return the hid_dev object itself as the "interface".
     * The pipe query functions will cast it back. */
    return &hid_dev->object;
}
WDFFUNC(WdfUsbTargetDeviceGetInterface, 236)

__winfnc BYTE WdfUsbInterfaceGetConfiguredSettingIndex(WDF_DRIVER_GLOBALS *globals, WDFOBJECT usb_if_obj) {
    log_info("WdfUsbInterfaceGetConfiguredSettingIndex: ENTER (if=%p)", usb_if_obj);
    return 0;
}
WDFFUNC(WdfUsbInterfaceGetConfiguredSettingIndex, 237)

__winfnc void WdfUsbInterfaceGetDescriptor(WDF_DRIVER_GLOBALS *globals, WDFOBJECT usb_if_obj, UCHAR setting_idx, USB_INTERFACE_DESCRIPTOR *descriptor) {
    log_info("WdfUsbInterfaceGetDescriptor: ENTER (if=%p, setting=%d)", usb_if_obj, setting_idx);
    *descriptor = (USB_INTERFACE_DESCRIPTOR) {
        .bLength = sizeof(USB_INTERFACE_DESCRIPTOR),
        .bDescriptorType = 4,  /* INTERFACE */
        .bInterfaceNumber = 0,
        .bAlternateSetting = 0,
        .bNumEndpoints = 2,
        .bInterfaceClass = 0xFF,  /* vendor-specific */
        .bInterfaceSubClass = 0,
        .bInterfaceProtocol = 0,
        .iInterface = 0
    };
}
WDFFUNC(WdfUsbInterfaceGetDescriptor, 232)

__winfnc BYTE WdfUsbInterfaceGetNumEndpoints(WDF_DRIVER_GLOBALS *globals, WDFOBJECT usb_if_obj, UCHAR setting_idx) {
    log_info("WdfUsbInterfaceGetNumEndpoints: ENTER (if=%p, setting=%d)", usb_if_obj, setting_idx);
    return 2;  /* OUT and IN */
}
WDFFUNC(WdfUsbInterfaceGetNumEndpoints, 231)

__winfnc WDFOBJECT WdfUsbInterfaceGetConfiguredPipe(WDF_DRIVER_GLOBALS *globals, WDFOBJECT usb_if_obj, UCHAR idx, WDF_USB_PIPE_INFORMATION *info) {
    log_info("WdfUsbInterfaceGetConfiguredPipe: ENTER (if=%p, idx=%d)", usb_if_obj, idx);
    struct wdf_hid_device *hid_dev = (struct wdf_hid_device*) usb_if_obj;

    if (idx >= hid_dev->num_pipes) return NULL;
    struct wdf_hid_pipe *pipe = &hid_dev->pipes[idx];

    if (info) {
        info->Size = sizeof(WDF_USB_PIPE_INFORMATION);
        info->MaximumPacketSize = pipe->is_input ? HID_REPORT_IN_SIZE : HID_REPORT_OUT_SIZE;
        info->EndpointAddress = pipe->endpoint_addr;
        info->Interval = 1;
        info->SettingIndex = 0;
        info->PipeType = WdfUsbPipeTypeInterrupt;
        info->MaximumTransferSize = 0;
    }

    return &pipe->object;
}
WDFFUNC(WdfUsbInterfaceGetConfiguredPipe, 239)

__winfnc void WdfUsbTargetPipeSetNoMaximumPacketSizeCheck(WDF_DRIVER_GLOBALS *globals, WDFOBJECT usb_pipe_obj) {
    log_info("WdfUsbTargetPipeSetNoMaximumPacketSizeCheck: ENTER (pipe=%p)", usb_pipe_obj);
}
WDFFUNC(WdfUsbTargetPipeSetNoMaximumPacketSizeCheck, 220)

__winfnc NTSTATUS WdfUsbTargetPipeAbortSynchronously(WDF_DRIVER_GLOBALS *globals, WDFOBJECT usb_pipe_obj, WDFOBJECT request, WDF_REQUEST_SEND_OPTIONS *req_opts) {
    log_info("WdfUsbTargetPipeAbortSynchronously: ENTER (pipe=%p)", usb_pipe_obj);
    return STATUS_SUCCESS;
}
WDFFUNC(WdfUsbTargetPipeAbortSynchronously, 226)

__winfnc NTSTATUS WdfUsbTargetPipeResetSynchronously(WDF_DRIVER_GLOBALS *globals, WDFOBJECT usb_pipe_obj, WDFOBJECT request, WDF_REQUEST_SEND_OPTIONS *req_opts) {
    /* Re-initialize the HID sensor */
    struct wdf_hid_pipe *pipe = (struct wdf_hid_pipe*) usb_pipe_obj;
    log_info("WdfUsbTargetPipeResetSynchronously: ENTER (pipe=%p, re-initializing HID sensor)", usb_pipe_obj);
    hid_init_sensor(pipe->hid_dev->hidraw_fd);
    return STATUS_SUCCESS;
}
WDFFUNC(WdfUsbTargetPipeResetSynchronously, 228)

/* ========== Async transfer handling ========== */

struct hid_transfer_ctx {
    struct wdf_memory *mem;
    WDFMEMORY_OFFSET mem_off;
    struct winwdf_request *request;

    struct wdf_hid_pipe *pipe;
    int timeout_ms;

    pthread_t worker_thread;
    bool cancelled;
};

#define USBD_STATUS_SUCCESS 0x00000000
#define USBD_STATUS_ERROR 0xc0000000
#define USBD_STATUS_TIMEOUT 0xc0006000

static void *hid_write_worker(void *arg) {
    struct hid_transfer_ctx *ctx = (struct hid_transfer_ctx*) arg;
    struct wdf_hid_device *hid_dev = ctx->pipe->hid_dev;

    uint8_t *data = ctx->mem->data + ctx->mem_off.BufferOffset;
    size_t len = ctx->mem_off.BufferLength;

    log_info("hid_write_worker: ENTER (len=%zu, fd=%d, tid=%lu)", len, hid_dev->hidraw_fd, (unsigned long)pthread_self());
    log_debug("HID write: %zu bytes of raw VCSFW data", len);
    if (len >= 2) log_debug("  VCSFW cmd: 0x%02x%02x", data[1], data[0]);

    log_info("hid_write_worker: acquiring io_lock...");
    cant_fail_ret(pthread_mutex_lock(&hid_dev->io_lock));
    log_info("hid_write_worker: io_lock acquired, calling hid_send_vcsfw...");
    int ret = hid_send_vcsfw(hid_dev->hidraw_fd, data, len);
    log_info("hid_write_worker: hid_send_vcsfw returned %d", ret);
    cant_fail_ret(pthread_mutex_unlock(&hid_dev->io_lock));

    if (ctx->cancelled) {
        wdf_complete_request(ctx->request, STATUS_CANCELLED, NULL);
        return NULL;
    }

    NTSTATUS status = (ret == 0) ? STATUS_SUCCESS : WINERR_SET_CODE;

    WDF_USB_REQUEST_COMPLETION_PARAMS usb_params;
    usb_params.UsbdStatus = (status == STATUS_SUCCESS) ? USBD_STATUS_SUCCESS : USBD_STATUS_ERROR;
    usb_params.Type = WdfUsbRequestTypePipeWrite;
    usb_params.Parameters.PipeWrite.Buffer = &ctx->mem->object;
    usb_params.Parameters.PipeWrite.Length = (ret == 0) ? len : 0;
    usb_params.Parameters.PipeWrite.Offset = ctx->mem_off.BufferOffset;

    WDF_REQUEST_COMPLETION_PARAMS params;
    params.Size = sizeof(WDF_REQUEST_COMPLETION_PARAMS);
    params.Type = WdfRequestTypeUsb;
    params.IoStatus.Status = status;
    params.IoStatus.Information = (ret == 0) ? len : 0;
    params.Parameters.Usb.Completion = &usb_params;

    wdf_complete_request(ctx->request, status, &params);
    return NULL;
}

static void *hid_read_worker(void *arg) {
    struct hid_transfer_ctx *ctx = (struct hid_transfer_ctx*) arg;
    struct wdf_hid_device *hid_dev = ctx->pipe->hid_dev;

    uint8_t *data = ctx->mem->data + ctx->mem_off.BufferOffset;
    size_t buf_size = ctx->mem_off.BufferLength;

    log_info("hid_read_worker: ENTER (buf_size=%zu, timeout=%d, fd=%d, tid=%lu)", buf_size, ctx->timeout_ms, hid_dev->hidraw_fd, (unsigned long)pthread_self());
    log_info("hid_read_worker: acquiring io_lock...");
    cant_fail_ret(pthread_mutex_lock(&hid_dev->io_lock));
    log_info("hid_read_worker: io_lock acquired, calling hid_recv_vcsfw...");
    ssize_t n = hid_recv_vcsfw(hid_dev->hidraw_fd, data, buf_size, ctx->timeout_ms);
    cant_fail_ret(pthread_mutex_unlock(&hid_dev->io_lock));

    if (ctx->cancelled) {
        wdf_complete_request(ctx->request, STATUS_CANCELLED, NULL);
        return NULL;
    }

    NTSTATUS status = (n > 0) ? STATUS_SUCCESS : WINERR_SET_CODE;

    if (n > 0) {
        log_debug("HID read: %zd bytes of VCSFW response", n);
        if (n >= 2) log_debug("  VCSFW status: 0x%04x", (unsigned)(data[0] | (data[1] << 8)));
    } else {
        log_debug("HID read: failed/timeout");
    }

    WDF_USB_REQUEST_COMPLETION_PARAMS usb_params;
    usb_params.UsbdStatus = (status == STATUS_SUCCESS) ? USBD_STATUS_SUCCESS : USBD_STATUS_TIMEOUT;
    usb_params.Type = WdfUsbRequestTypePipeRead;
    usb_params.Parameters.PipeRead.Buffer = &ctx->mem->object;
    usb_params.Parameters.PipeRead.Length = (n > 0) ? n : 0;
    usb_params.Parameters.PipeRead.Offset = ctx->mem_off.BufferOffset;

    WDF_REQUEST_COMPLETION_PARAMS params;
    params.Size = sizeof(WDF_REQUEST_COMPLETION_PARAMS);
    params.Type = WdfRequestTypeUsb;
    params.IoStatus.Status = status;
    params.IoStatus.Information = (n > 0) ? n : 0;
    params.Parameters.Usb.Completion = &usb_params;

    wdf_complete_request(ctx->request, status, &params);
    return NULL;
}

static NTSTATUS hid_transfer_start(struct winwdf_request *req, struct hid_transfer_ctx *ctx, WDFOBJECT target, int timeout, void **data) {
    log_info("hid_transfer_start: ENTER (req=%p, pipe=%s, timeout=%d)", req, ctx->pipe->is_input ? "IN/read" : "OUT/write", timeout);
    *data = NULL;
    ctx->timeout_ms = (timeout > 0) ? timeout : 5000;
    ctx->cancelled = false;

    /* Start worker thread */
    void *(*worker_fn)(void*) = ctx->pipe->is_input ? hid_read_worker : hid_write_worker;
    int err = pthread_create(&ctx->worker_thread, NULL, worker_fn, ctx);
    if (err) {
        log_error("Failed to create HID worker thread: %d", err);
        return WINERR_SET_CODE;
    }
    pthread_detach(ctx->worker_thread);

    return STATUS_SUCCESS;
}

static void hid_transfer_cancel(struct winwdf_request *req, struct hid_transfer_ctx *ctx, void *data) {
    ctx->cancelled = true;
}

static void hid_transfer_cleanup(struct winwdf_request *req, struct hid_transfer_ctx *ctx, void *data) {
    free(ctx);
}

__winfnc NTSTATUS WdfUsbTargetDeviceFormatRequestForControlTransfer(WDF_DRIVER_GLOBALS *globals, WDFOBJECT usb_dev_obj, WDFOBJECT req_obj, WDF_USB_CONTROL_SETUP_PACKET *packet, WDFOBJECT mem_obj, WDFMEMORY_OFFSET *mem_off) {
    /* Control transfers are not meaningful over HID - stub it */
    log_info("WdfUsbTargetDeviceFormatRequestForControlTransfer: ENTER (usb_dev=%p, req=%p) - not supported over HID, stubbing", usb_dev_obj, req_obj);
    return STATUS_SUCCESS;
}
WDFFUNC(WdfUsbTargetDeviceFormatRequestForControlTransfer, 213)

__winfnc NTSTATUS WdfUsbTargetPipeFormatRequestForRead(WDF_DRIVER_GLOBALS *globals, WDFOBJECT usb_pipe_obj, WDFOBJECT req_obj, WDFOBJECT mem_obj, WDFMEMORY_OFFSET *mem_off) {
    log_info("WdfUsbTargetPipeFormatRequestForRead: ENTER (pipe=%p, req=%p, mem=%p)", usb_pipe_obj, req_obj, mem_obj);
    struct wdf_hid_pipe *pipe = (struct wdf_hid_pipe*) usb_pipe_obj;
    struct wdf_memory *mem = (struct wdf_memory*) mem_obj;
    struct winwdf_request *req = (struct winwdf_request*) req_obj;

    if (!pipe->is_input) {
        log_warn("Attempted HID read request for non-IN pipe!");
        return WINERR_SET_CODE;
    }

    struct hid_transfer_ctx *ctx = (struct hid_transfer_ctx*) malloc(sizeof(struct hid_transfer_ctx));
    if (!ctx) return winerr_from_errno();

    ctx->mem = mem;
    ctx->mem_off = mem_off ? *mem_off : ((WDFMEMORY_OFFSET) { .BufferOffset = 0, .BufferLength = mem->data_size });
    ctx->request = req;
    ctx->pipe = pipe;
    ctx->timeout_ms = 5000;

    wdf_configure_request(req, NULL, ctx, 0, 0, (wdf_request_start_fnc*) hid_transfer_start, (wdf_request_cancel_fnc*) hid_transfer_cancel, (wdf_request_cleanup_fnc*) hid_transfer_cleanup, NULL);

    return STATUS_SUCCESS;
}
WDFFUNC(WdfUsbTargetPipeFormatRequestForRead, 224)

__winfnc NTSTATUS WdfUsbTargetPipeFormatRequestForWrite(WDF_DRIVER_GLOBALS *globals, WDFOBJECT usb_pipe_obj, WDFOBJECT req_obj, WDFOBJECT mem_obj, WDFMEMORY_OFFSET *mem_off) {
    log_info("WdfUsbTargetPipeFormatRequestForWrite: ENTER (pipe=%p, req=%p, mem=%p)", usb_pipe_obj, req_obj, mem_obj);
    struct wdf_hid_pipe *pipe = (struct wdf_hid_pipe*) usb_pipe_obj;
    struct wdf_memory *mem = (struct wdf_memory*) mem_obj;
    struct winwdf_request *req = (struct winwdf_request*) req_obj;

    if (pipe->is_input) {
        log_warn("Attempted HID write request for non-OUT pipe!");
        return WINERR_SET_CODE;
    }

    struct hid_transfer_ctx *ctx = (struct hid_transfer_ctx*) malloc(sizeof(struct hid_transfer_ctx));
    if (!ctx) return winerr_from_errno();

    ctx->mem = mem;
    ctx->mem_off = mem_off ? *mem_off : ((WDFMEMORY_OFFSET) { .BufferOffset = 0, .BufferLength = mem->data_size });
    ctx->request = req;
    ctx->pipe = pipe;
    ctx->timeout_ms = 5000;

    wdf_configure_request(req, NULL, ctx, 0, 0, (wdf_request_start_fnc*) hid_transfer_start, (wdf_request_cancel_fnc*) hid_transfer_cancel, (wdf_request_cleanup_fnc*) hid_transfer_cleanup, NULL);

    return STATUS_SUCCESS;
}
WDFFUNC(WdfUsbTargetPipeFormatRequestForWrite, 222)
