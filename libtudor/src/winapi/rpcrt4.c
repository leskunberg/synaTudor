/*
 * RPCRT4.dll function stubs
 *
 * UuidFromStringW - parse a UUID string into a GUID.
 */

#include <stdio.h>
#include <string.h>
#include "internal.h"

#define RPC_S_OK 0
#define RPC_S_INVALID_STRING_UUID 1705

__winfnc DWORD UuidFromStringW(const char16_t *str, GUID *uuid) {
    if(!str || !uuid) return RPC_S_INVALID_STRING_UUID;

    /* Convert to ASCII for sscanf */
    char buf[64];
    int i;
    for(i = 0; i < 63 && str[i]; i++) {
        buf[i] = (char)(str[i] & 0x7F);
    }
    buf[i] = '\0';

    /* Parse UUID: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx */
    unsigned int a;
    unsigned int b, c;
    unsigned int d0, d1;
    unsigned int e0, e1, e2, e3, e4, e5;
    int n = sscanf(buf, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        &a, &b, &c, &d0, &d1, &e0, &e1, &e2, &e3, &e4, &e5);
    if(n != 11) {
        log_warn("UuidFromStringW: failed to parse '%s'", buf);
        return RPC_S_INVALID_STRING_UUID;
    }

    uuid->PartA = a;
    uuid->PartB = (uint16_t)b;
    uuid->PartC = (uint16_t)c;
    uuid->PartD = ((d0 & 0xFF) << 0) | ((d1 & 0xFF) << 8);
    uuid->PartE = ((uint64_t)(e0 & 0xFF) << 0) | ((uint64_t)(e1 & 0xFF) << 8) |
                  ((uint64_t)(e2 & 0xFF) << 16) | ((uint64_t)(e3 & 0xFF) << 24) |
                  ((uint64_t)(e4 & 0xFF) << 32) | ((uint64_t)(e5 & 0xFF) << 40);

    log_debug("UuidFromStringW: parsed '%s' -> {%08x-%04x-%04x-...}", buf, uuid->PartA, uuid->PartB, uuid->PartC);
    return RPC_S_OK;
}
WINAPI(UuidFromStringW)
