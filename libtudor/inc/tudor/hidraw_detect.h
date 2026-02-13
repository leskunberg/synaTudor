#ifndef TUDOR_HIDRAW_DETECT_H
#define TUDOR_HIDRAW_DETECT_H

#include <stdbool.h>
#include <stddef.h>

#define HIDRAW_PATH_MAX 32

struct hidraw_detect_result {
    char cmd_path[HIDRAW_PATH_MAX];
    char img_path[HIDRAW_PATH_MAX];
    bool found;
};

/*
 * Scan /sys/class/hidraw/ for the Lenovo keyboard fingerprint sensor.
 * Matches VID:PID 17EF:613E (BT) or 17EF:6142 (USB).
 * Identifies command channel (reports 0x0E/0F/10/11 under UP 0xFF00)
 * and image channel (reports 0x0C/0D/20/21).
 * In BT mode both channels are on the same hidraw device.
 */
bool hidraw_autodetect(struct hidraw_detect_result *result);

/*
 * Check if a hidraw device (by sysfs name like "hidraw4") is the
 * fingerprint command channel. Returns true if VID:PID matches our
 * keyboard and report descriptor contains cmd reports 0x0E/0F/10/11.
 */
bool hidraw_is_fp_command_channel(const char *hidraw_name);

/*
 * Given a hidraw command channel path (e.g. "/dev/hidraw4"), find its
 * image channel partner (same parent HID device, different interface).
 * Returns true and fills img_path if found, false otherwise.
 */
bool hidraw_find_image_partner(const char *cmd_path, char *img_path, size_t img_path_max);

#endif
