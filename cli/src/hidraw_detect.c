#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <tudor/log.h>
#include "hidraw_detect.h"

/* Lenovo X1 Fold 16 keyboard VID:PID */
#define KEYBOARD_VID 0x17EF
#define KEYBOARD_PID_BT  0x613E
#define KEYBOARD_PID_USB 0x6142

/* HID bus types from linux/hid.h */
#define BUS_USB   0x0003
#define BUS_BT    0x0005

/* Report IDs for fingerprint command channel (Usage Page 0xFF00) */
#define FP_CMD_REPORT_OUT    0x0E
#define FP_CMD_REPORT_IN     0x0F
#define FP_CMD_REPORT_NOTIFY 0x10
#define FP_CMD_REPORT_FEAT   0x11

/* Report IDs for fingerprint image channel */
#define FP_IMG_REPORT_FEAT1  0x0C
#define FP_IMG_REPORT_FEAT2  0x0D
#define FP_IMG_REPORT_OUT    0x20
#define FP_IMG_REPORT_IN     0x21

struct rdesc_info {
    bool has_cmd_reports;  /* has 0x0E, 0x0F, 0x10, 0x11 under UP 0xFF00 */
    bool has_img_reports;  /* has 0x0C, 0x0D, 0x20, 0x21 */
};

/*
 * Minimal HID report descriptor parser.
 * Walks items tracking the current Usage Page. When we see Report ID items,
 * we check if the fingerprint report IDs are present under the right Usage Page.
 */
static void parse_report_descriptor(const uint8_t *desc, size_t len, struct rdesc_info *info) {
    uint16_t current_usage_page = 0;
    bool seen_cmd[4] = {false};  /* 0x0E, 0x0F, 0x10, 0x11 */
    bool seen_img[4] = {false};  /* 0x0C, 0x0D, 0x20, 0x21 */

    size_t i = 0;
    while(i < len) {
        uint8_t prefix = desc[i];

        /* Long item */
        if(prefix == 0xFE) {
            if(i + 1 >= len) break;
            uint8_t data_size = desc[i + 1];
            i += 3 + data_size;
            continue;
        }

        /* Short item: size from bits 0-1 */
        int data_size = prefix & 0x03;
        if(data_size == 3) data_size = 4;
        uint8_t tag = prefix & 0xFC;  /* tag + type */

        if(i + 1 + data_size > len) break;

        uint32_t value = 0;
        for(int j = 0; j < data_size; j++)
            value |= (uint32_t)desc[i + 1 + j] << (j * 8);

        /* Usage Page (Global, tag = 0x04/0x05/0x06) */
        if(tag == 0x04 || tag == 0x05 || tag == 0x06) {
            current_usage_page = (uint16_t)value;
        }

        /* Report ID (Global, tag = 0x84/0x85) */
        if(tag == 0x84 || tag == 0x85) {
            uint8_t rid = (uint8_t)value;

            /* Command channel reports must be under Usage Page 0xFF00 */
            if(current_usage_page == 0xFF00) {
                if(rid == FP_CMD_REPORT_OUT)    seen_cmd[0] = true;
                if(rid == FP_CMD_REPORT_IN)     seen_cmd[1] = true;
                if(rid == FP_CMD_REPORT_NOTIFY) seen_cmd[2] = true;
                if(rid == FP_CMD_REPORT_FEAT)   seen_cmd[3] = true;
            }

            /* Image channel reports can be under 0xFF00 or 0x00FF */
            if(current_usage_page == 0xFF00 || current_usage_page == 0x00FF) {
                if(rid == FP_IMG_REPORT_FEAT1)  seen_img[0] = true;
                if(rid == FP_IMG_REPORT_FEAT2)  seen_img[1] = true;
                if(rid == FP_IMG_REPORT_OUT)    seen_img[2] = true;
                if(rid == FP_IMG_REPORT_IN)     seen_img[3] = true;
            }
        }

        i += 1 + data_size;
    }

    info->has_cmd_reports = seen_cmd[0] && seen_cmd[1] && seen_cmd[2] && seen_cmd[3];
    info->has_img_reports = seen_img[0] && seen_img[1] && seen_img[2] && seen_img[3];
}

/*
 * Read HID_ID from /sys/class/hidraw/hidrawN/device/uevent.
 * Format: HID_ID=BBBB:VVVVVVVV:PPPPPPPP (bus:vendor:product, all hex)
 * Returns true if VID:PID matches our keyboard.
 */
static bool check_hid_id(const char *hidraw_name) {
    char path[256];
    snprintf(path, sizeof(path), "/sys/class/hidraw/%s/device/uevent", hidraw_name);

    FILE *f = fopen(path, "r");
    if(!f) return false;

    char line[256];
    bool match = false;
    while(fgets(line, sizeof(line), f)) {
        unsigned int bus, vid, pid;
        if(sscanf(line, "HID_ID=%x:%x:%x", &bus, &vid, &pid) == 3) {
            if(vid == KEYBOARD_VID &&
               (pid == KEYBOARD_PID_BT || pid == KEYBOARD_PID_USB)) {
                match = true;
                log_debug("hidraw_detect: %s matches (bus=0x%04x vid=0x%04x pid=0x%04x)",
                          hidraw_name, bus, vid, pid);
            }
            break;
        }
    }
    fclose(f);
    return match;
}

/*
 * Read and parse the report descriptor for a hidraw device.
 */
static bool check_report_descriptor(const char *hidraw_name, struct rdesc_info *info) {
    char path[256];
    snprintf(path, sizeof(path), "/sys/class/hidraw/%s/device/report_descriptor", hidraw_name);

    int fd = open(path, O_RDONLY);
    if(fd < 0) return false;

    uint8_t buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf));
    close(fd);

    if(n <= 0) return false;

    parse_report_descriptor(buf, (size_t)n, info);
    return true;
}

bool hidraw_autodetect(struct hidraw_detect_result *result) {
    memset(result, 0, sizeof(*result));

    DIR *dir = opendir("/sys/class/hidraw");
    if(!dir) {
        log_warn("hidraw_detect: cannot open /sys/class/hidraw");
        return false;
    }

    char cmd_device[HIDRAW_PATH_MAX] = {0};
    char img_device[HIDRAW_PATH_MAX] = {0};

    struct dirent *ent;
    while((ent = readdir(dir)) != NULL) {
        if(strncmp(ent->d_name, "hidraw", 6) != 0) continue;

        if(!check_hid_id(ent->d_name)) continue;

        struct rdesc_info info = {0};
        if(!check_report_descriptor(ent->d_name, &info)) continue;

        char devpath[HIDRAW_PATH_MAX];
        snprintf(devpath, sizeof(devpath), "/dev/%.25s", ent->d_name);

        if(info.has_cmd_reports && !cmd_device[0]) {
            strncpy(cmd_device, devpath, HIDRAW_PATH_MAX - 1);
            log_info("hidraw_detect: command channel -> %s", devpath);
        }
        if(info.has_img_reports && !img_device[0]) {
            strncpy(img_device, devpath, HIDRAW_PATH_MAX - 1);
            log_info("hidraw_detect: image channel   -> %s", devpath);
        }
    }
    closedir(dir);

    if(!cmd_device[0]) {
        log_warn("hidraw_detect: no fingerprint sensor found");
        return false;
    }

    strncpy(result->cmd_path, cmd_device, HIDRAW_PATH_MAX - 1);
    strncpy(result->img_path, img_device[0] ? img_device : cmd_device, HIDRAW_PATH_MAX - 1);
    result->found = true;

    log_info("hidraw_detect: auto-detected cmd=%s img=%s", result->cmd_path, result->img_path);
    return true;
}
