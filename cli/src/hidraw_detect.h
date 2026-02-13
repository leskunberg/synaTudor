#ifndef HIDRAW_DETECT_H
#define HIDRAW_DETECT_H

#include <stdbool.h>

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

#endif
