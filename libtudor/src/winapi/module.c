#include <pthread.h>
#include "internal.h"

static pthread_rwlock_t modules_lock = PTHREAD_RWLOCK_INITIALIZER;
static struct winmodule *modules_head;

static void module_destr(struct winmodule *module) {
    module->handle = NULL;
    if(module->cmdline) return;

    //Free the module
    winmodule_unregister(module);
    free((void*) module->name);
    free(module);
}

struct winmodule *winmodule_find(const char *name) {
    cant_fail_ret(pthread_rwlock_rdlock(&modules_lock));

    struct winmodule *module = NULL;
    for(struct winmodule *m = modules_head; m != NULL; m = m->next) {
        if(strcmp(m->name, name) == 0) {
            module = m;
            break;
        }
    }

    cant_fail_ret(pthread_rwlock_unlock(&modules_lock));
    return module;
}

struct winmodule *winmodule_find_by_addr(const void *addr) {
    cant_fail_ret(pthread_rwlock_rdlock(&modules_lock));

    struct winmodule *module = NULL;
    for(struct winmodule *m = modules_head; m != NULL; m = m->next) {
        if(m->image_base && m->image_size > 0) {
            uintptr_t base = (uintptr_t)m->image_base;
            if((uintptr_t)addr >= base && (uintptr_t)addr < base + m->image_size) {
                module = m;
                break;
            }
        }
    }

    cant_fail_ret(pthread_rwlock_unlock(&modules_lock));
    return module;
}

void winmodule_register(struct winmodule *module) {
    cant_fail_ret(pthread_rwlock_wrlock(&modules_lock));

    module->handle = winhandle_create(module, (winhandle_destr_fnc*) module_destr);

    module->prev = NULL;
    module->next = modules_head;
    if(modules_head) modules_head->prev = module;
    modules_head = module;

    cant_fail_ret(pthread_rwlock_unlock(&modules_lock));
}

void winmodule_unregister(struct winmodule *module) {
    cant_fail_ret(pthread_rwlock_wrlock(&modules_lock));

    if(module->handle) {
        winhandle_destroy(module->handle);
        module->handle = NULL;
    }

    if(module->prev) module->prev->next = module->next;
    else modules_head = module->next;
    if(module->next) module->next->prev = module->prev;

    cant_fail_ret(pthread_rwlock_unlock(&modules_lock));
}

static __thread struct winmodule *cur_module;

struct winmodule *winmodule_get_cur() {
    WIN_CLOBBER_NONVOL_REGS
    return cur_module;
}

void winmodule_set_cur(struct winmodule *module) {
    cur_module = module;
}

__winfnc HANDLE LoadLibraryExW(const char16_t *name, HANDLE file, DWORD flags) {
    //We don't support loading librarys dynamically, but some stdlib functions have to be loaded that way
    //As such return dummy modules
    struct winmodule *module = (struct winmodule*) malloc(sizeof(struct winmodule));
    if(!module) { winerr_set_errno(); return NULL; }
    *module = (struct winmodule) {0};
    module->name = winstr_to_str(name);
    winmodule_register(module);
    return module->handle;
}
WINAPI(LoadLibraryExW)

__winfnc BOOL FreeLibrary(HANDLE handle) {
    winhandle_destroy(handle);
    return TRUE;
}
WINAPI(FreeLibrary)

__winfnc HANDLE GetModuleHandleA(const char *name) {
    if(!name) {
        struct winmodule *module = winmodule_get_cur();
        return module ? module->handle : NULL;
    }
    struct winmodule *module = (struct winmodule*) winmodule_find(name);
    return module ? module->handle : NULL;
}
WINAPI(GetModuleHandleA)

__winfnc HANDLE GetModuleHandleW(const char16_t *name) {
    if(!name) {
        struct winmodule *module = winmodule_get_cur();
        return module ? module->handle : NULL;
    }
    char *cname = winstr_to_str(name);
    struct winmodule *module = (struct winmodule*) winmodule_find(cname);
    free(cname);
    return module ? module->handle : NULL;
}
WINAPI(GetModuleHandleW)

#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 0x00000004
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 0x00000002

struct winmodule *winmodule_find_by_addr(const void *addr);

__winfnc BOOL GetModuleHandleExW(DWORD flags, const char16_t *name, HANDLE *out) {
    if(flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) {
        struct winmodule *module = winmodule_find_by_addr((const void *)name);
        if(module) {
            log_debug("GetModuleHandleExW: resolved addr %p to module '%s'", name, module->name);
            if(out) *out = module->handle;
            return TRUE;
        }
        log_warn("GetModuleHandleExW: couldn't resolve addr %p to any module", name);
        if(out) *out = NULL;
        return FALSE;
    }

    char *cname = winstr_to_str(name);
    struct winmodule *module = (struct winmodule*) winmodule_find(cname);
    free(cname);

    if(module && out) *out = module->handle;
    return module != NULL;
}
WINAPI(GetModuleHandleExW)

__winfnc DWORD GetModuleFileNameA(HANDLE handle, char *name, DWORD size) {
    struct winmodule *module = handle ? (struct winmodule*) handle->data : cur_module;
 
    int name_len = strlen(module->name);

    if(size == 0) {
        winerr_set_code(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    } else if(name_len+1 <= size) {
        memcpy(name, module->name, name_len+1);
        winerr_clear();
        return name_len;
    } else {
        name_len = size - 1;
        memcpy(name, module->name, name_len+1);
        winerr_set_code(ERROR_INSUFFICIENT_BUFFER);
        return name_len;
    }
}
WINAPI(GetModuleFileNameA)

__winfnc DWORD GetModuleFileNameW(HANDLE handle, char16_t *name, DWORD size) {
    struct winmodule *module = handle ? (struct winmodule*) handle->data : cur_module;
 
    char16_t *wname = winstr_from_str(module->name);
    int wname_len = winstr_len(wname);

    if(size == 0) {
        free(wname);
        winerr_set_code(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    } else if(wname_len+1 <= size / sizeof(char16_t)) {
        memcpy(name, wname, (wname_len+1) * sizeof(char16_t));
        winerr_clear();
        free(wname);
        return wname_len;
    } else {
        wname_len = size / sizeof(char16_t) - 1;
        memcpy(name, wname, (wname_len+1) * sizeof(char16_t));
        winerr_set_code(ERROR_INSUFFICIENT_BUFFER);
        free(wname);
        return wname_len;
    }
}
WINAPI(GetModuleFileNameW)

__winfnc BOOL DisableThreadLibraryCalls(HANDLE handle) {
    return TRUE;
}
WINAPI(DisableThreadLibraryCalls)

__winfnc HANDLE LoadLibraryA(const char *name) {
    struct winmodule *module = (struct winmodule*) malloc(sizeof(struct winmodule));
    if(!module) { winerr_set_errno(); return NULL; }
    *module = (struct winmodule) {0};
    module->name = strdup(name);
    winmodule_register(module);
    return module->handle;
}
WINAPI(LoadLibraryA)

__winfnc void FreeLibraryAndExitThread(HANDLE module, DWORD exit_code) {
    log_debug("FreeLibraryAndExitThread called (module=%p, exit_code=%u)", module, exit_code);
    FreeLibrary(module);
    pthread_exit((void*)(uintptr_t)exit_code);
}
WINAPI(FreeLibraryAndExitThread)

__winfnc void *GetProcAddress(HANDLE handle, const char *name) {
    struct winmodule *module = (struct winmodule*) handle->data;

    //Check if it's an ordinal import
    uintptr_t nbits = (uintptr_t) name;
    if(!(nbits & ~((128ull << sizeof(uintptr_t)) - 1))) {
        int ord = (int) (nbits & ((128ull << sizeof(uintptr_t)) - 1));
        log_warn("GetProcAddress: Attempted ordinal import! module '%s' [%p] ord %d", module->name, module, ord);
        return NULL;
    }

    //Try to resolve the Windows API function
    void *resolv = resolve_windows_api(name);
    if(!resolv) winerr_set();
    return resolv;
}
WINAPI(GetProcAddress)