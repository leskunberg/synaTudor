/*
 * SETUPAPI.dll / cfgmgr32 function stubs for synaWudfBioHid153.dll
 *
 * The DLL uses CM_* functions to enumerate device interfaces and
 * find the HID device path. We return fake device info pointing to
 * our sensor.
 */

#include <string.h>
#include "internal.h"

/* Configuration Manager return codes */
#define CR_SUCCESS 0x00000000
#define CR_BUFFER_SMALL 0x0000001A
#define CR_NO_SUCH_DEVNODE 0x0000000D
#define CR_INVALID_POINTER 0x00000002

/* DEVINST is a handle to a device instance (just an integer) */
typedef DWORD DEVINST;
typedef DWORD CONFIGRET;

/* Fake device instance IDs */
static const char fake_device_id_a[] = "HID\\VID_06CB&PID_00DD\\TUDOR_FP";
static const char16_t fake_device_id_w[] = u"HID\\VID_06CB&PID_00DD\\TUDOR_FP";

/* Parent device (USB composite) */
static const char fake_parent_id_a[] = "USB\\VID_17EF&PID_6142\\TUDOR_KB";
static const char16_t fake_parent_id_w[] = u"USB\\VID_17EF&PID_6142\\TUDOR_KB";

/* Fake device interface paths.
 * The DLL enumerates these via CM_Get_Device_Interface_ListW, opens each
 * with CreateFileW, and identifies them by HidP_GetCaps (report sizes).
 * We provide two interfaces:
 *   MI_04 = command channel (hidraw4: reports 0x0E/0x0F/0x10/0x11)
 *   MI_01 = image channel   (hidraw1: reports 0x0C/0x0D/0x20/0x21)
 */
static const char fake_iface_cmd_a[] = "\\\\?\\HID#VID_17EF&PID_6142&MI_04#7&00000001&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}";
static const char16_t fake_iface_cmd_w[] = u"\\\\?\\HID#VID_17EF&PID_6142&MI_04#7&00000001&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}";
static const char fake_iface_img_a[] = "\\\\?\\HID#VID_17EF&PID_6142&MI_01#7&00000001&0&0001#{4d1e55b2-f16f-11cf-88cb-001111000030}";
static const char16_t fake_iface_img_w[] = u"\\\\?\\HID#VID_17EF&PID_6142&MI_01#7&00000001&0&0001#{4d1e55b2-f16f-11cf-88cb-001111000030}";

#define FAKE_DEVINST 0x12345678
#define FAKE_PARENT_DEVINST 0x12345600

__winfnc CONFIGRET CM_Locate_DevNodeW(DEVINST *dn_inst, char16_t *dev_id, ULONG flags) {
    log_debug("CM_Locate_DevNodeW called");
    if (dn_inst) *dn_inst = FAKE_DEVINST;
    return CR_SUCCESS;
}
WINAPI(CM_Locate_DevNodeW)

__winfnc CONFIGRET CM_Get_Parent(DEVINST *dn_inst, DEVINST dn_child, ULONG flags) {
    log_debug("CM_Get_Parent called (child=0x%x)", dn_child);
    if (dn_inst) *dn_inst = FAKE_PARENT_DEVINST;
    return CR_SUCCESS;
}
WINAPI(CM_Get_Parent)

__winfnc CONFIGRET CM_Get_Device_ID_Size(ULONG *size, DEVINST dn_inst, ULONG flags) {
    const char *id = (dn_inst == FAKE_DEVINST) ? fake_device_id_a : fake_parent_id_a;
    log_debug("CM_Get_Device_ID_Size called (inst=0x%x) -> %s", dn_inst, id);
    if (size) *size = (ULONG)strlen(id);
    return CR_SUCCESS;
}
WINAPI(CM_Get_Device_ID_Size)

__winfnc CONFIGRET CM_Get_Device_IDW(DEVINST dn_inst, char16_t *buf, ULONG buf_len, ULONG flags) {
    const char16_t *id;
    size_t len_chars; /* string length in chars, NOT including null */
    if (dn_inst == FAKE_DEVINST) {
        id = fake_device_id_w;
        len_chars = sizeof(fake_device_id_w) / sizeof(char16_t) - 1;
    } else {
        id = fake_parent_id_w;
        len_chars = sizeof(fake_parent_id_w) / sizeof(char16_t) - 1;
    }
    log_info("CM_Get_Device_IDW called (inst=0x%x, buf_len=%u, need=%zu)", dn_inst, buf_len, len_chars);
    if (!buf) return CR_INVALID_POINTER;
    /* buf_len is in characters; DLL passes ID length without null
     * and null-terminates the buffer itself after the call */
    if (buf_len < len_chars) return CR_BUFFER_SMALL;
    memcpy(buf, id, len_chars * sizeof(char16_t));
    /* Also write null if there's room */
    if (buf_len > len_chars) buf[len_chars] = u'\0';
    return CR_SUCCESS;
}
WINAPI(CM_Get_Device_IDW)

__winfnc CONFIGRET CM_Get_Device_Interface_List_SizeA(ULONG *size, GUID *iface_guid, char *dev_id, ULONG flags) {
    log_debug("CM_Get_Device_Interface_List_SizeA called");
    /* Multi-string: path1\0 + path2\0 + \0 */
    if (size) *size = (ULONG)(strlen(fake_iface_cmd_a) + 1 + strlen(fake_iface_img_a) + 1 + 1);
    return CR_SUCCESS;
}
WINAPI(CM_Get_Device_Interface_List_SizeA)

__winfnc CONFIGRET CM_Get_Device_Interface_List_SizeW(ULONG *size, GUID *iface_guid, char16_t *dev_id, ULONG flags) {
    char *dev_id_str = dev_id ? winstr_to_str(dev_id) : NULL;
    /* Multi-string: path1\0 + path2\0 + \0 (in char16_t units) */
    ULONG cmd_chars = sizeof(fake_iface_cmd_w) / sizeof(char16_t); /* includes null */
    ULONG img_chars = sizeof(fake_iface_img_w) / sizeof(char16_t); /* includes null */
    ULONG result_size = cmd_chars + img_chars + 1; /* +1 for final null */
    log_info("CM_Get_Device_Interface_List_SizeW called (dev_id='%s') -> size=%u chars (%u cmd + %u img)",
             dev_id_str ? dev_id_str : "(null)", result_size, cmd_chars, img_chars);
    free(dev_id_str);
    if (size) *size = result_size;
    return CR_SUCCESS;
}
WINAPI(CM_Get_Device_Interface_List_SizeW)

__winfnc CONFIGRET CM_Get_Device_Interface_ListA(GUID *iface_guid, char *dev_id, char *buf, ULONG buf_len, ULONG flags) {
    log_debug("CM_Get_Device_Interface_ListA called");
    if (!buf) return CR_INVALID_POINTER;
    size_t cmd_len = strlen(fake_iface_cmd_a);
    size_t img_len = strlen(fake_iface_img_a);
    size_t need = cmd_len + 1 + img_len + 1 + 1; /* path1\0 + path2\0 + \0 */
    if (buf_len < need) return CR_BUFFER_SMALL;
    memcpy(buf, fake_iface_cmd_a, cmd_len + 1);
    memcpy(buf + cmd_len + 1, fake_iface_img_a, img_len + 1);
    buf[need - 1] = '\0';
    return CR_SUCCESS;
}
WINAPI(CM_Get_Device_Interface_ListA)

__winfnc CONFIGRET CM_Get_Device_Interface_ListW(GUID *iface_guid, char16_t *dev_id, char16_t *buf, ULONG buf_len, ULONG flags) {
    char *dev_id_str = dev_id ? winstr_to_str(dev_id) : NULL;
    log_info("CM_Get_Device_Interface_ListW called (dev_id='%s', buf_len=%u)", dev_id_str ? dev_id_str : "(null)", buf_len);
    free(dev_id_str);
    if (!buf) return CR_INVALID_POINTER;
    size_t cmd_chars = sizeof(fake_iface_cmd_w) / sizeof(char16_t); /* includes null */
    size_t img_chars = sizeof(fake_iface_img_w) / sizeof(char16_t); /* includes null */
    size_t need = cmd_chars + img_chars + 1; /* +1 for final null */
    if (buf_len < need) return CR_BUFFER_SMALL;
    memcpy(buf, fake_iface_cmd_w, sizeof(fake_iface_cmd_w));
    memcpy(buf + cmd_chars, fake_iface_img_w, sizeof(fake_iface_img_w));
    buf[need - 1] = u'\0';
    log_info("CM_Get_Device_Interface_ListW: returned 2 interfaces (cmd + img)");
    return CR_SUCCESS;
}
WINAPI(CM_Get_Device_Interface_ListW)

/* CM_Get_Device_Interface_PropertyW from api-ms-win-devices-config-l1-1-1.dll */
#define DEVPROP_TYPE_STRING 0x00000012
typedef struct {
    ULONG fmtid_data1;
    USHORT fmtid_data2;
    USHORT fmtid_data3;
    UCHAR fmtid_data4[8];
    ULONG pid;
} DEVPROPKEY;

__winfnc CONFIGRET CM_Get_Device_Interface_PropertyW(const char16_t *dev_iface, const DEVPROPKEY *propkey, ULONG *prop_type, BYTE *buf, ULONG *buf_size, ULONG flags) {
    log_debug("CM_Get_Device_Interface_PropertyW called");
    /* Return the device ID as a default property */
    if (prop_type) *prop_type = DEVPROP_TYPE_STRING;
    size_t need = sizeof(fake_device_id_w);
    if (buf_size && *buf_size >= need && buf) {
        memcpy(buf, fake_device_id_w, need);
    }
    if (buf_size) *buf_size = (ULONG)need;
    return CR_SUCCESS;
}
WINAPI(CM_Get_Device_Interface_PropertyW)
