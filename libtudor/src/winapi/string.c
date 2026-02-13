#include <limits.h>
#include "internal.h"

#define MAX_DEFAULTCHAR 2
#define MAX_LEADBYTES 12

typedef struct {
    UINT MaxCharSize;
    BYTE DefaultChar[MAX_DEFAULTCHAR];
    BYTE LeadByte[MAX_LEADBYTES];
} CPINFO;

__winfnc UINT GetACP() {
    return 1200; //UTF-16
}
WINAPI(GetACP)

__winfnc BOOL IsValidCodePage(UINT code_page) {
    switch(code_page) {
        case 65001: return TRUE; //UTF-8
        case 1200: return TRUE; //UTF-16
        case 12000: return TRUE; //UTF-32
        default: return FALSE;
    }
}
WINAPI(IsValidCodePage)

__winfnc BOOL GetCPInfo(UINT code_page, CPINFO *info) {
    switch(code_page) {
        case 65001: *info = (CPINFO) { .MaxCharSize = 1, .DefaultChar = { '?' } }; return TRUE; //UTF-8
        case 1200: *info = (CPINFO) { .MaxCharSize = 2, .DefaultChar = { '?' } }; return TRUE; //UTF-16
        case 12000: *info = (CPINFO) { .MaxCharSize = 4, .DefaultChar = { '?' } }; return TRUE; //UTF-32
        default: return FALSE;
    }
}
WINAPI(GetCPInfo)

__winfnc int MultiByteToWideChar(UINT code_page, DWORD flags, const char *mbstr, int mblen, char16_t *wstr, int wlen) {
    if(!mbstr) { winerr_set(); return 0; }

    int include_null = 0;
    if(mblen < 0) {
        mblen = (int)strlen(mbstr);
        include_null = 1;
    }
    if(mblen == 0 && !include_null) { winerr_set(); return 0; }

    /* Count required wide chars */
    mbstate_t mbs = {0};
    int wchar_count = 0;
    const char *p = mbstr;
    int remaining = mblen;
    while(remaining > 0) {
        char16_t tmp;
        int n = (int)mbrtoc16(&tmp, p, remaining, &mbs);
        if(n <= 0) { n = 1; } /* skip invalid bytes */
        wchar_count++;
        p += n;
        remaining -= n;
    }
    if(include_null) wchar_count++;

    /* If wlen == 0, return required size */
    if(wlen == 0) return wchar_count;
    if(wlen < wchar_count) { winerr_set(); return 0; }

    /* Convert */
    mbs = (mbstate_t){0};
    p = mbstr;
    remaining = mblen;
    int wi = 0;
    while(remaining > 0) {
        int n = (int)mbrtoc16(&wstr[wi], p, remaining, &mbs);
        if(n <= 0) { wstr[wi] = (char16_t)(unsigned char)*p; n = 1; }
        wi++;
        p += n;
        remaining -= n;
    }
    if(include_null) wstr[wi] = 0;

    return wchar_count;
}
WINAPI(MultiByteToWideChar)

__winfnc int WideCharToMultiByte(UINT code_page, DWORD flags, const char16_t *wstr, int wlen, char *mbstr, int mblen, const char *default_char, BOOL *used_default) {
    if(!wstr) { winerr_set(); return 0; }

    int include_null = 0;
    if(wlen < 0) {
        wlen = winstr_len(wstr);
        include_null = 1;
    }
    if(wlen == 0 && !include_null) { winerr_set(); return 0; }

    /* Count required bytes */
    mbstate_t mbs = {0};
    int total = 0;
    for(int i = 0; i < wlen; i++) {
        char buf[MB_LEN_MAX];
        int n = (int)c16rtomb(buf, wstr[i], &mbs);
        if(n < 0) { n = 1; } /* count 1 for unconvertible chars */
        total += n;
    }
    if(include_null) total++;

    /* If mblen == 0, return required size */
    if(mblen == 0) return total;
    if(mblen < total) { winerr_set(); return 0; }

    /* Convert */
    mbs = (mbstate_t){0};
    int written = 0;
    for(int i = 0; i < wlen; i++) {
        int n = (int)c16rtomb(mbstr + written, wstr[i], &mbs);
        if(n < 0) { mbstr[written] = '?'; n = 1; }
        written += n;
    }
    if(include_null) mbstr[written++] = '\0';

    return written;
}
WINAPI(WideCharToMultiByte)

__winfnc int GetStringTypeW(DWORD info_type, const char16_t *str, int strlen, WORD* char_types) {
    if(strlen < 0) strlen = winstr_len(str);

    //TODO
    memset(char_types, 0, strlen * sizeof(WORD));
    return 0;
}
WINAPI(GetStringTypeW)

__winfnc int LCMapStringW(DWORD lcid, DWORD flags, const char16_t *in, int inlen, char16_t *out, int outlen) {
    if(inlen < 0) inlen = winstr_len(in);
    if(outlen == 0) return (inlen + 1) * sizeof(char16_t);

    //TODO
    if(outlen < inlen) {
        winerr_set();
        return 0;
    }

    memcpy(out, in, (inlen + 1) * sizeof(char16_t));
    return outlen;
}
WINAPI(LCMapStringW)

__winfnc void RtlInitUnicodeString(UNICODE_STRING *dst, const char16_t *src) {
    if(src) {
        int len = winstr_len(src);
        dst->Length = dst->MaximumLength = len+1;
        dst->Buffer = (char16_t*) malloc((len+1) * sizeof(char16_t));
        if(!dst->Buffer) { perror("Couldn't allocate UNICODE_STRING buffer"); abort(); }
        memcpy(dst->Buffer, src, (len+1) * sizeof(char16_t));
    } else {
        dst->Length = dst->MaximumLength = 0;
        dst->Buffer = NULL;
    }
}
WINAPI(RtlInitUnicodeString)

__winfnc int lstrlenA(const char *str) {
    return (int) strlen(str);
}
WINAPI(lstrlenA)

__winfnc char16_t *lstrcpynW(char16_t *dst, const char16_t *src, int max_len) {
    for(; *src && max_len > 0; src++, dst++, max_len--) *dst = *src;
    *dst = 0;
    return dst;
}
WINAPI(lstrcpynW);

__winfnc int lstrcmpW(const char16_t *a, const char16_t *b) {
    int a_len = winstr_len(a), b_len = winstr_len(b);
    if(a_len < b_len) return -1;
    if(a_len > b_len) return -1;
    return memcmp(a, b, a_len * sizeof(char16_t));
}
WINAPI(lstrcmpW);

__winfnc int lstrlenW(const char16_t *str) {
    if(!str) return 0;
    return winstr_len(str);
}
WINAPI(lstrlenW)

__winfnc char *lstrcpyA(char *dst, const char *src) {
    return strcpy(dst, src);
}
WINAPI(lstrcpyA)

__winfnc char16_t *lstrcpyW(char16_t *dst, const char16_t *src) {
    char16_t *d = dst;
    while((*d++ = *src++));
    return dst;
}
WINAPI(lstrcpyW)

#define CSTR_LESS_THAN 1
#define CSTR_EQUAL 2
#define CSTR_GREATER_THAN 3

__winfnc int CompareStringW(DWORD locale, DWORD flags, const char16_t *str1, int cch1, const char16_t *str2, int cch2) {
    if(cch1 < 0) cch1 = winstr_len(str1);
    if(cch2 < 0) cch2 = winstr_len(str2);
    int len = cch1 < cch2 ? cch1 : cch2;
    int cmp = memcmp(str1, str2, len * sizeof(char16_t));
    if(cmp < 0) return CSTR_LESS_THAN;
    if(cmp > 0) return CSTR_GREATER_THAN;
    if(cch1 < cch2) return CSTR_LESS_THAN;
    if(cch1 > cch2) return CSTR_GREATER_THAN;
    return CSTR_EQUAL;
}
WINAPI(CompareStringW)

__winfnc UINT GetOEMCP() {
    return 437; /* US OEM code page */
}
WINAPI(GetOEMCP)