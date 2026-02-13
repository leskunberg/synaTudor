/*
 * ADVAPI32.dll security stubs for synaWudfBioHid153.dll
 *
 * SID allocation, security descriptors, and ACL functions.
 * Used by the DLL to set up security on sensor data storage.
 */

#include <string.h>
#include "internal.h"

/* SID structures */
typedef struct {
    BYTE Revision;
    BYTE SubAuthorityCount;
    BYTE IdentifierAuthority[6];
    DWORD SubAuthority[8];
} SID;

/* Minimal security descriptor (opaque 40-byte struct) */
typedef struct {
    BYTE data[40];
} SECURITY_DESCRIPTOR;

/* ACL type */
typedef struct {
    BYTE data[8];
} ACL;

__winfnc BOOL AllocateAndInitializeSid(void *authority, BYTE sub_authority_count,
    DWORD a0, DWORD a1, DWORD a2, DWORD a3, DWORD a4, DWORD a5, DWORD a6, DWORD a7,
    void **sid) {
    log_debug("AllocateAndInitializeSid called");
    SID *s = (SID*) malloc(sizeof(SID));
    if(!s) { winerr_set_errno(); return FALSE; }
    memset(s, 0, sizeof(SID));
    s->Revision = 1;
    s->SubAuthorityCount = sub_authority_count;
    if(authority) memcpy(s->IdentifierAuthority, authority, 6);
    if(sub_authority_count > 0) s->SubAuthority[0] = a0;
    if(sub_authority_count > 1) s->SubAuthority[1] = a1;
    if(sub_authority_count > 2) s->SubAuthority[2] = a2;
    if(sub_authority_count > 3) s->SubAuthority[3] = a3;
    *sid = s;
    return TRUE;
}
WINAPI(AllocateAndInitializeSid)

__winfnc void *FreeSid(void *sid) {
    free(sid);
    return NULL;
}
WINAPI(FreeSid)

__winfnc BOOL InitializeSecurityDescriptor(void *sd, DWORD revision) {
    log_debug("InitializeSecurityDescriptor called (rev=%u)", revision);
    if(sd) memset(sd, 0, sizeof(SECURITY_DESCRIPTOR));
    return TRUE;
}
WINAPI(InitializeSecurityDescriptor)

__winfnc BOOL SetSecurityDescriptorDacl(void *sd, BOOL dacl_present, void *dacl, BOOL dacl_defaulted) {
    log_debug("SetSecurityDescriptorDacl called");
    return TRUE;
}
WINAPI(SetSecurityDescriptorDacl)

__winfnc DWORD SetEntriesInAclA(ULONG count, void *entries, void *old_acl, void **new_acl) {
    log_debug("SetEntriesInAclA called (count=%u)", count);
    /* Return a dummy ACL */
    ACL *a = (ACL*) malloc(sizeof(ACL));
    if(!a) return winerr_from_errno();
    memset(a, 0, sizeof(ACL));
    if(new_acl) *new_acl = a;
    return ERROR_SUCCESS;
}
WINAPI(SetEntriesInAclA)
