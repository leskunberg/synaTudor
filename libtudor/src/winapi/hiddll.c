/*
 * HID.DLL function implementations for synaWudfBioHid153.dll
 *
 * These functions are called by the HID driver DLL to communicate with
 * the fingerprint sensor via HID reports. We route them to the hidraw fd.
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <linux/hidraw.h>
#include "internal.h"

/* HID attributes structure matching Windows HIDD_ATTRIBUTES */
typedef struct {
    ULONG Size;
    USHORT VendorID;
    USHORT ProductID;
    USHORT VersionNumber;
} HIDD_ATTRIBUTES;

/* Opaque preparsed data handle */
typedef void *PHIDP_PREPARSED_DATA;

/* HID capabilities */
typedef struct {
    USHORT Usage;
    USHORT UsagePage;
    USHORT InputReportByteLength;
    USHORT OutputReportByteLength;
    USHORT FeatureReportByteLength;
    USHORT Reserved[17];
    USHORT NumberLinkCollectionNodes;
    USHORT NumberInputButtonCaps;
    USHORT NumberInputValueCaps;
    USHORT NumberInputDataIndices;
    USHORT NumberOutputButtonCaps;
    USHORT NumberOutputValueCaps;
    USHORT NumberOutputDataIndices;
    USHORT NumberFeatureButtonCaps;
    USHORT NumberFeatureValueCaps;
    USHORT NumberFeatureDataIndices;
} HIDP_CAPS;

/* HID value caps */
typedef struct {
    USHORT UsagePage;
    UCHAR ReportID;
    BOOLEAN IsAlias;
    USHORT BitField;
    USHORT LinkCollection;
    USHORT LinkUsage;
    USHORT LinkUsagePage;
    BOOLEAN IsRange;
    BOOLEAN IsStringRange;
    BOOLEAN IsDesignatorRange;
    BOOLEAN IsAbsolute;
    BOOLEAN HasNull;
    UCHAR Reserved;
    USHORT BitSize;
    USHORT ReportCount;
    USHORT Reserved2[5];
    ULONG UnitsExp;
    ULONG Units;
    LONG LogicalMin;
    LONG LogicalMax;
    LONG PhysicalMin;
    LONG PhysicalMax;
    union {
        struct {
            USHORT UsageMin;
            USHORT UsageMax;
            USHORT StringMin;
            USHORT StringMax;
            USHORT DesignatorMin;
            USHORT DesignatorMax;
            USHORT DataIndexMin;
            USHORT DataIndexMax;
        } Range;
        struct {
            USHORT Usage;
            USHORT Reserved1;
            USHORT StringIndex;
            USHORT Reserved2;
            USHORT DesignatorIndex;
            USHORT Reserved3;
            USHORT DataIndex;
            USHORT Reserved4;
        } NotRange;
    };
} HIDP_VALUE_CAPS;

/* HIDP_STATUS codes */
#define HIDP_STATUS_SUCCESS 0x00110000

/* HidP_Report_Type */
typedef enum {
    HidP_Input = 0,
    HidP_Output = 1,
    HidP_Feature = 2
} HIDP_REPORT_TYPE;

/* Preparsed data tokens - different values for different interfaces */
static int preparsed_cmd = 0xFEED;
static int preparsed_img = 0xBEEF;

/* Handle-to-interface mapping */
#define MAX_HID_HANDLES 8
static struct { void *handle; int iface_type; } hid_handle_table[MAX_HID_HANDLES];
static int hid_handle_count = 0;

void hid_register_handle(void *handle, int iface_type) {
    if(hid_handle_count < MAX_HID_HANDLES) {
        hid_handle_table[hid_handle_count].handle = handle;
        hid_handle_table[hid_handle_count].iface_type = iface_type;
        hid_handle_count++;
        log_info("hid_register_handle: handle=%p type=%s", handle,
                 iface_type == HID_IFACE_IMG ? "IMAGE" : "CMD");
    }
}

int hid_get_handle_type(void *handle) {
    for(int i = 0; i < hid_handle_count; i++) {
        if(hid_handle_table[i].handle == handle) return hid_handle_table[i].iface_type;
    }
    return HID_IFACE_CMD;
}

int hid_get_fd_for_handle(void *handle) {
    int type = hid_get_handle_type(handle);
    if(type == HID_IFACE_IMG && win_hidraw_fd_img >= 0)
        return win_hidraw_fd_img;
    return win_hidraw_fd;
}

__winfnc BOOLEAN HidD_GetAttributes(HANDLE device, HIDD_ATTRIBUTES *attrs) {
    log_debug("HidD_GetAttributes called");
    if (!attrs) return FALSE;
    attrs->Size = sizeof(HIDD_ATTRIBUTES);
    attrs->VendorID = 0x06cb;   /* Synaptics */
    attrs->ProductID = 0x00dd;  /* Tudor HID */
    attrs->VersionNumber = 0x0100;
    return TRUE;
}
WINAPI(HidD_GetAttributes)

__winfnc BOOLEAN HidD_GetPreparsedData(HANDLE device, PHIDP_PREPARSED_DATA *ppd) {
    int type = hid_get_handle_type(device);
    log_info("HidD_GetPreparsedData called (handle=%p, type=%s)", device,
             type == HID_IFACE_IMG ? "IMAGE" : "CMD");
    if (!ppd) return FALSE;
    *ppd = (type == HID_IFACE_IMG) ? &preparsed_img : &preparsed_cmd;
    return TRUE;
}
WINAPI(HidD_GetPreparsedData)

__winfnc BOOLEAN HidD_FreePreparsedData(PHIDP_PREPARSED_DATA ppd) {
    log_debug("HidD_FreePreparsedData called");
    return TRUE;
}
WINAPI(HidD_FreePreparsedData)

__winfnc NTSTATUS HidP_GetCaps(PHIDP_PREPARSED_DATA ppd, HIDP_CAPS *caps) {
    bool is_img = (ppd == &preparsed_img);
    log_info("HidP_GetCaps called (iface=%s)", is_img ? "IMAGE" : "CMD");
    if (!caps) return WINERR_SET_CODE;
    memset(caps, 0, sizeof(HIDP_CAPS));
    caps->Usage = 1;
    caps->UsagePage = 0xFF00;  /* Vendor-defined */
    if (is_img) {
        /* Image channel: hidraw1 - reports 0x0C/0x0D(feature,64B), 0x20(output,64B), 0x21(input,64B) */
        caps->InputReportByteLength = 64;   /* 1 report_id + 63 data (0x21) */
        caps->OutputReportByteLength = 64;  /* 1 report_id + 63 data (0x20) */
        caps->FeatureReportByteLength = 64; /* 1 report_id + 63 data (0x0C/0x0D) */
        caps->NumberInputValueCaps = 1;     /* Report 0x21 */
        caps->NumberOutputValueCaps = 1;    /* Report 0x20 */
        caps->NumberFeatureValueCaps = 2;   /* Reports 0x0C and 0x0D */
    } else {
        /* Command channel: hidraw4 - reports 0x0E(output,20B), 0x0F(input,64B), 0x10(input,20B), 0x11(feature,20B) */
        caps->InputReportByteLength = 64;   /* 1 report_id + 63 data (0x0F) */
        caps->OutputReportByteLength = 20;  /* 1 report_id + 19 data (0x0E) */
        caps->FeatureReportByteLength = 20; /* 1 report_id + 19 data (0x11) */
        caps->NumberInputValueCaps = 2;     /* Reports 0x0F and 0x10 */
        caps->NumberOutputValueCaps = 1;    /* Report 0x0E */
        caps->NumberFeatureValueCaps = 1;   /* Report 0x11 */
    }
    return HIDP_STATUS_SUCCESS;
}
WINAPI(HidP_GetCaps)

__winfnc NTSTATUS HidP_GetValueCaps(HIDP_REPORT_TYPE type, HIDP_VALUE_CAPS *caps, USHORT *caps_len, PHIDP_PREPARSED_DATA ppd) {
    bool is_img = (ppd == &preparsed_img);
    log_debug("HidP_GetValueCaps called (type=%d, iface=%s)", type, is_img ? "IMAGE" : "CMD");
    if (!caps || !caps_len) return WINERR_SET_CODE;

    if (is_img) {
        /* Image channel reports */
        switch (type) {
        case HidP_Input:
            if (*caps_len >= 1) {
                memset(caps, 0, sizeof(HIDP_VALUE_CAPS));
                caps[0].UsagePage = 0xFF00;
                caps[0].ReportID = 0x21;
                caps[0].BitSize = 8;
                caps[0].ReportCount = 63;
                *caps_len = 1;
            } else { *caps_len = 0; }
            break;
        case HidP_Output:
            if (*caps_len >= 1) {
                memset(caps, 0, sizeof(HIDP_VALUE_CAPS));
                caps[0].UsagePage = 0xFF00;
                caps[0].ReportID = 0x20;
                caps[0].BitSize = 8;
                caps[0].ReportCount = 63;
                *caps_len = 1;
            } else { *caps_len = 0; }
            break;
        case HidP_Feature:
            if (*caps_len >= 2) {
                memset(caps, 0, 2 * sizeof(HIDP_VALUE_CAPS));
                caps[0].UsagePage = 0xFF00;
                caps[0].ReportID = 0x0C;
                caps[0].BitSize = 8;
                caps[0].ReportCount = 63;
                caps[1].UsagePage = 0xFF00;
                caps[1].ReportID = 0x0D;
                caps[1].BitSize = 8;
                caps[1].ReportCount = 63;
                *caps_len = 2;
            } else { *caps_len = 0; }
            break;
        }
    } else {
        /* Command channel reports */
        switch (type) {
        case HidP_Input:
            if (*caps_len >= 2) {
                memset(caps, 0, 2 * sizeof(HIDP_VALUE_CAPS));
                caps[0].UsagePage = 0xFF00;
                caps[0].ReportID = 0x0F;
                caps[0].BitSize = 8;
                caps[0].ReportCount = 63;
                caps[1].UsagePage = 0xFF00;
                caps[1].ReportID = 0x10;
                caps[1].BitSize = 8;
                caps[1].ReportCount = 19;
                *caps_len = 2;
            } else { *caps_len = 0; }
            break;
        case HidP_Output:
            if (*caps_len >= 1) {
                memset(caps, 0, sizeof(HIDP_VALUE_CAPS));
                caps[0].UsagePage = 0xFF00;
                caps[0].ReportID = 0x0E;
                caps[0].BitSize = 8;
                caps[0].ReportCount = 19;
                *caps_len = 1;
            } else { *caps_len = 0; }
            break;
        case HidP_Feature:
            if (*caps_len >= 1) {
                memset(caps, 0, sizeof(HIDP_VALUE_CAPS));
                caps[0].UsagePage = 0xFF00;
                caps[0].ReportID = 0x11;
                caps[0].BitSize = 8;
                caps[0].ReportCount = 19;
                *caps_len = 1;
            } else { *caps_len = 0; }
            break;
        }
    }
    return HIDP_STATUS_SUCCESS;
}
WINAPI(HidP_GetValueCaps)

/* Global hidraw fds set during device creation */
int win_hidraw_fd = -1;
int win_hidraw_fd_img = -1;

/*
 * SSI (Sensor Serial Interface) scan cycle via Feature Report 0x0C.
 *
 * On Windows, the KMDF filter driver (SynapticsFingerprintIntegration.sys)
 * performs this cycle on the image channel (USB interface MI_01) to trigger
 * the sensor's image capture hardware. Without it, the sensor never captures
 * a real fingerprint image, causing identify/verify to always fail.
 *
 * The cycle is: INIT → READ_REG → CONFIGURE → READ_REGs → START_SCAN
 * Each operation is a SET_FEATURE + GET_FEATURE pair on Report 0x0C (64 bytes).
 *
 * Observed from pcapng captures of the Windows driver:
 * - GET_FEATURE always returns status=0xFA (success sentinel)
 * - READ_REG values are always 0xFAFAFAFA (passthrough/sentinel)
 * - The sequence is identical for both identify and enrollment
 */
static bool ssi_feature_0c(int fd, uint8_t subcmd, uint16_t param, const char *label) {
    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));
    buf[0] = 0x0C;     /* report ID */
    buf[1] = subcmd;
    buf[2] = param & 0xFF;         /* param lo */
    buf[3] = (param >> 8) & 0xFF;  /* param hi */

    /* SET_FEATURE */
    int ret = ioctl(fd, HIDIOCSFEATURE(64), buf);
    if(ret < 0) {
        log_warn("ssi_feature_0c: SET %s failed: %s", label, strerror(errno));
        return false;
    }

    /* GET_FEATURE to read response */
    memset(buf, 0, sizeof(buf));
    buf[0] = 0x0C;
    ret = ioctl(fd, HIDIOCGFEATURE(64), buf);
    if(ret < 0) {
        log_warn("ssi_feature_0c: GET %s failed: %s", label, strerror(errno));
        return false;
    }

    log_info("ssi_feature_0c: %s → subcmd=0x%02x status=0x%02x data=[%02x %02x %02x %02x]",
             label, buf[1], buf[2], buf[3], buf[4], buf[5], buf[6]);
    return true;
}

static void ssi_run_scan_cycle(int fd) {
    if(fd < 0) {
        log_warn("ssi_run_scan_cycle: no image channel fd available — sensor capture may fail!");
        return;
    }
    log_info("ssi_run_scan_cycle: starting on fd=%d", fd);

    ssi_feature_0c(fd, 0x04, 0x0000, "INIT");
    ssi_feature_0c(fd, 0x0F, 0x804A, "READ_REG(0x804A)");
    ssi_feature_0c(fd, 0x03, 0x0000, "CONFIGURE");
    ssi_feature_0c(fd, 0x0F, 0xFF58, "READ_REG(0xFF58)");
    ssi_feature_0c(fd, 0x0F, 0x325E, "READ_REG(0x325E)");
    ssi_feature_0c(fd, 0x0F, 0x1C3C, "READ_REG(0x1C3C)");
    ssi_feature_0c(fd, 0x0F, 0x323D, "READ_REG(0x323D)");
    ssi_feature_0c(fd, 0x80, 0x0000, "START_SCAN");

    log_info("ssi_run_scan_cycle: complete — sensor image pipeline active");
}

__winfnc BOOLEAN HidD_GetFeature(HANDLE device, void *report_buf, ULONG report_len) {
    uint8_t *rb = (uint8_t*)report_buf;
    int fd = hid_get_fd_for_handle(device);
    log_debug("HidD_GetFeature called (len=%u, report_id=0x%02x, fd=%d)", report_len, rb ? rb[0] : 0, fd);
    if(fd < 0) {
        log_warn("HidD_GetFeature: no hidraw fd available");
        return FALSE;
    }
    log_debug("HidD_GetFeature: calling ioctl...");
    int ret = ioctl(fd, HIDIOCGFEATURE(report_len), report_buf);
    if(ret < 0) {
        log_warn("HidD_GetFeature: ioctl failed: %s", strerror(errno));
        return FALSE;
    }
    {
        char hex[128];
        int hlen = 0;
        for(int i = 0; i < ret && i < 20 && hlen < (int)sizeof(hex)-4; i++)
            hlen += snprintf(hex + hlen, sizeof(hex) - hlen, "%02x ", rb[i]);
        log_info("HidD_GetFeature: got %d bytes [%s] (fd=%d)", ret, hex, fd);
    }
    return TRUE;
}
WINAPI(HidD_GetFeature)

__winfnc BOOLEAN HidD_SetFeature(HANDLE device, void *report_buf, ULONG report_len) {
    uint8_t *rb = (uint8_t*)report_buf;
    int fd = hid_get_fd_for_handle(device);
    {
        char hex[128];
        int hlen = 0;
        for(ULONG i = 0; i < report_len && i < 20 && hlen < (int)sizeof(hex)-4; i++)
            hlen += snprintf(hex + hlen, sizeof(hex) - hlen, "%02x ", rb[i]);
        log_info("HidD_SetFeature called (len=%u, report_id=0x%02x, fd=%d) [%s]", report_len, rb ? rb[0] : 0, fd, hex);
    }
    if(fd < 0) {
        log_warn("HidD_SetFeature: no hidraw fd available");
        return FALSE;
    }
    int ret = ioctl(fd, HIDIOCSFEATURE(report_len), report_buf);
    if(ret < 0) {
        log_warn("HidD_SetFeature: ioctl failed: %s", strerror(errno));
        return FALSE;
    }
    log_debug("HidD_SetFeature: sent %d bytes", ret);

    /*
     * On Linux hidraw, the sensor sends its response as an INPUT report
     * (report 0x0F or 0x10), unlike Windows where SetFeature responses
     * go back through the control pipe. Consume any immediate responses
     * so they don't confuse the DLL's ReadFile-based reader thread.
     *
     * Only drain on the COMMAND channel fd — the image channel uses
     * Feature reports bidirectionally and doesn't produce stale input reports.
     */
    if(fd == win_hidraw_fd) {
        bool is_wait_event = (report_len >= 5 && rb[1] == 'w' && rb[2] == 'a' && rb[3] == 'i' && rb[4] == 't');
        if(is_wait_event) {
            log_info("HidD_SetFeature: 'wait event' detected — enabling heartbeat pass-through");
            win_hid_pass_heartbeats = true;
            ssi_run_scan_cycle(win_hidraw_fd_img);
            start_img_monitor(win_hidraw_fd_img);
            /* No drain — let all events reach hid_read_thread and the DLL.
             * The sensor sends alternating heartbeats that are indistinguishable
             * from real events. "end event" stops them between cycles, and
             * TLS reads consume any stale reports from the buffer. */
        }
        bool is_end_event = (report_len >= 4 && rb[1] == 'e' && rb[2] == 'n' && rb[3] == 'd');
        if(is_end_event && win_hid_pass_heartbeats) {
            log_info("HidD_SetFeature: 'end event' detected — disabling heartbeat pass-through");
            win_hid_pass_heartbeats = false;
        }
        /* Drain stale responses for non-wait-event commands (e.g., "end event",
         * synaptic init commands). Short timeout to catch immediate acks only. */
        if(!is_wait_event) {
            int drain_timeout = 50;
            struct pollfd pfd = { .fd = fd, .events = POLLIN };
            uint8_t drain[64];
            int drained = 0;
            int non_fp_skipped = 0;
            int save_flags = fcntl(fd, F_GETFL);
            while(poll(&pfd, 1, drain_timeout) > 0 && (pfd.revents & POLLIN)) {
                fcntl(fd, F_SETFL, save_flags | O_NONBLOCK);
                ssize_t n = read(fd, drain, sizeof(drain));
                fcntl(fd, F_SETFL, save_flags);
                if(n > 0) {
                    if(drain[0] != 0x0F && drain[0] != 0x10 && drain[0] != 0x11) {
                        non_fp_skipped++;
                        continue;
                    }
                    char hex[80];
                    int hlen = 0;
                    for(int i = 0; i < n && i < 20; i++)
                        hlen += snprintf(hex + hlen, sizeof(hex) - hlen, "%02x ", drain[i]);
                    log_info("HidD_SetFeature: consumed ack %zd bytes [%s]", n, hex);
                    drained++;
                } else break;
            }
            if(non_fp_skipped > 0)
                log_info("HidD_SetFeature: skipped %d non-FP report(s) during drain", non_fp_skipped);
            if(drained > 0)
                log_info("HidD_SetFeature: drained %d response(s) from input channel", drained);
        }
    }

    return TRUE;
}
WINAPI(HidD_SetFeature)

__winfnc void HidD_GetHidGuid(GUID *guid) {
    log_debug("HidD_GetHidGuid called");
    /* Windows HID class GUID: 4D1E55B2-F16F-11CF-88CB-001111000030 */
    if (guid) {
        *guid = DEFINE_GUID(4d1e55b2, f16f, 11cf, 88cb, 001111000030);
    }
}
WINAPI(HidD_GetHidGuid)

__winfnc BOOLEAN HidD_FlushQueue(HANDLE device) {
    int fd = hid_get_fd_for_handle(device);
    log_debug("HidD_FlushQueue called (fd=%d)", fd);
    if(fd < 0) return TRUE;

    /* Drain all pending input reports from the hidraw buffer */
    uint8_t drain[64];
    int count = 0;
    int flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    while(read(fd, drain, sizeof(drain)) > 0) {
        count++;
    }
    fcntl(fd, F_SETFL, flags);
    if(count > 0)
        log_info("HidD_FlushQueue: drained %d stale report(s) (fd=%d)", count, fd);
    return TRUE;
}
WINAPI(HidD_FlushQueue)
