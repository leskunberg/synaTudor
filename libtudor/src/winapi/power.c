/*
 * Power management function stubs for synaWudfBioHid153.dll
 *
 * api-ms-win-power-setting-l1-1-0.dll: PowerGetActiveScheme
 * POWRPROF.dll: CallNtPowerInformation, PowerReadACValueIndex, PowerReadDCValueIndex
 */

#include <string.h>
#include "internal.h"

#define ERROR_CALL_NOT_IMPLEMENTED 0x00000078

/* Fake power scheme GUID */
static GUID balanced_scheme = DEFINE_GUID(381b4222, f694, 41f0, 8682, 7b0609e633a9);

__winfnc DWORD PowerGetActiveScheme(void *user_root_key, GUID **active_policy_guid) {
    log_debug("PowerGetActiveScheme called");
    if (active_policy_guid) {
        GUID *g = (GUID *)malloc(sizeof(GUID));
        if (g) {
            *g = balanced_scheme;
            *active_policy_guid = g;
            return 0; /* ERROR_SUCCESS */
        }
    }
    return ERROR_CALL_NOT_IMPLEMENTED;
}
WINAPI(PowerGetActiveScheme)

/* SYSTEM_POWER_INFORMATION_LEVEL values */
__winfnc NTSTATUS CallNtPowerInformation(int level, void *in_buf, ULONG in_size, void *out_buf, ULONG out_size) {
    log_debug("CallNtPowerInformation called (level=%d)", level);
    if (out_buf && out_size > 0) memset(out_buf, 0, out_size);
    return STATUS_SUCCESS;
}
WINAPI(CallNtPowerInformation)

__winfnc DWORD PowerReadACValueIndex(void *root_key, const GUID *scheme, const GUID *subgroup, const GUID *setting, DWORD *value) {
    log_debug("PowerReadACValueIndex called");
    if (value) *value = 0;
    return 0;
}
WINAPI(PowerReadACValueIndex)

__winfnc DWORD PowerReadDCValueIndex(void *root_key, const GUID *scheme, const GUID *subgroup, const GUID *setting, DWORD *value) {
    log_debug("PowerReadDCValueIndex called");
    if (value) *value = 0;
    return 0;
}
WINAPI(PowerReadDCValueIndex)
