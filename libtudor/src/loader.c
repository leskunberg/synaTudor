#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <pthread.h>
#include <tudor/log.h>
#include "pe/pe.h"
#include "winapi/api.h"
#include "loader.h"
#include "stub.h"

/* CFG dispatch logging trampoline.
 * Replaces the PE's __guard_dispatch_icall_nop (jmp rax) with a version
 * that logs every indirect call target. This is essential for tracing
 * virtual function calls inside the DLL. */
static int cfg_log_count = 0;
void cfg_dispatch_log(void *target, void *ret_addr) {
    int n = __atomic_add_fetch(&cfg_log_count, 1, __ATOMIC_RELAXED);
    if(n <= 500 || (n % 10000) == 0) {
        log_debug("CFG dispatch #%d: target=%p [ret=%p] (tid=%lu)", n, target, ret_addr, (unsigned long)pthread_self());
    }
}

/* The trampoline is written as raw x86-64 machine code.
 * It saves all registers, calls cfg_dispatch_log, restores, and jmp rax.
 *
 * On entry (from DLL's `call [rip+...]`):
 *   - rsp+0: return address (pushed by call)
 *   - rax: target function address
 *   - rcx,rdx,r8,r9: call arguments (Windows x64)
 *   - rdi,rsi: may hold values (callee-saved in Windows x64)
 */
static void *create_cfg_trampoline(void) {
    uint8_t *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if(!page) { perror("mmap cfg trampoline"); abort(); }

    uint8_t *p = page;

    /* Save all potentially-used registers */
    *p++ = 0x50;                    /* push rax */
    *p++ = 0x51;                    /* push rcx */
    *p++ = 0x52;                    /* push rdx */
    *p++ = 0x41; *p++ = 0x50;      /* push r8 */
    *p++ = 0x41; *p++ = 0x51;      /* push r9 */
    *p++ = 0x41; *p++ = 0x52;      /* push r10 */
    *p++ = 0x41; *p++ = 0x53;      /* push r11 */
    *p++ = 0x57;                    /* push rdi */
    *p++ = 0x56;                    /* push rsi */
    /* 9 pushes = 72 bytes. Entry rsp was 8 mod 16, now rsp is (8-72)=(-64) mod 16 = 0 mod 16.
     * Before call, rsp should be 0 mod 16 (so callee sees 8 mod 16). Aligned! */

    /* mov rdi, rax  (System V arg1 = target address) */
    *p++ = 0x48; *p++ = 0x89; *p++ = 0xc7;

    /* mov rsi, [rsp+72]  (System V arg2 = return address, 9*8=72 bytes above) */
    *p++ = 0x48; *p++ = 0x8b; *p++ = 0x74; *p++ = 0x24; *p++ = 72;

    /* mov rax, <cfg_dispatch_log address> */
    uint64_t log_addr = (uint64_t)&cfg_dispatch_log;
    *p++ = 0x48; *p++ = 0xb8;
    memcpy(p, &log_addr, 8); p += 8;

    /* call rax */
    *p++ = 0xff; *p++ = 0xd0;

    /* Restore all registers */
    *p++ = 0x5e;                    /* pop rsi */
    *p++ = 0x5f;                    /* pop rdi */
    *p++ = 0x41; *p++ = 0x5b;      /* pop r11 */
    *p++ = 0x41; *p++ = 0x5a;      /* pop r10 */
    *p++ = 0x41; *p++ = 0x59;      /* pop r9 */
    *p++ = 0x41; *p++ = 0x58;      /* pop r8 */
    *p++ = 0x5a;                    /* pop rdx */
    *p++ = 0x59;                    /* pop rcx */
    *p++ = 0x58;                    /* pop rax */

    /* jmp rax */
    *p++ = 0xff; *p++ = 0xe0;

    log_debug("Created CFG dispatch trampoline at %p (%ld bytes)", page, (long)(p - page));
    return page;
}

/* RVA of the CFG dispatch function pointer in synaWudfBioHid153.dll.
 * This is __guard_dispatch_icall_fptr, which normally points to a `jmp rax` stub. */
#define CFG_DISPATCH_PTR_RVA 0xC67B8

static void patch_cfg_dispatch(uint8_t *image_mem, uint32_t image_size) {
    if(CFG_DISPATCH_PTR_RVA + 8 > image_size) {
        log_warn("CFG dispatch RVA out of bounds, skipping patch");
        return;
    }

    uint64_t *dispatch_ptr = (uint64_t*)(image_mem + CFG_DISPATCH_PTR_RVA);
    void *old_fn = (void*)*dispatch_ptr;
    void *trampoline = create_cfg_trampoline();
    *dispatch_ptr = (uint64_t)trampoline;
    log_info("Patched CFG dispatch: [0x%x] %p -> %p", CFG_DISPATCH_PTR_RVA, old_fn, trampoline);
}

void register_windows_api(char *name, void *api) {
    log_verbose("Registered Windows API function %s", name);
}

static void ord_stub() {
    log_error("Ordinal import called!");
    abort();
}

#ifndef DBGIMPORT
static void unresolved_stub() {
    log_error("Unresolved import called!");
    abort();
}
#endif

static void *resolve_import(const char *lib, const char *name) {
    //Try to resolve the import
    void *winapi = resolve_windows_api(name);
    if(winapi) return winapi;

    //Return a stub
    log_verbose("Couldn't resolve import %s@%s", name, lib);
#ifdef DBGIMPORT
    return create_import_stub(lib, name);
#else
    return &unresolved_stub;
#endif
}

bool load_dll(struct dll_image *dll, const char *name, uint8_t *data, uint32_t size) {
    //Parse the PE file
    struct pe_file pe;
    pe_parse(&pe, data, size);
    log_debug("DLL %s: %s image", name, pe.is_pe32_plus ? "PE+" : "PE");
    log_debug("-> machine: %x", pe.machine);
    log_debug("-> image size: %08x", pe.image_size);
    log_debug("-> entry point: %08x", pe.entry_point_off);
    log_debug("-> num data dirs: %d", pe.num_data_dirs);
    log_debug("-> num sections: %d", pe.num_sects);
    log_debug("-> num relocations: %d", pe.num_relocs);

    if(
        !(sizeof(void*) == 4 && pe.machine == PE_MACHINE_x86 && !pe.is_pe32_plus) &&
        !(sizeof(void*) == 8 && pe.machine == PE_MACHINE_x86_64 && pe.is_pe32_plus)
    ) {
        log_error("DLL target architecture incompatbile with host program!");
        return false;
    }

    //Map the image memory
    uint8_t *image_mem = mmap(NULL, pe.image_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if(!image_mem) {
        perror("Couldn't create DLL image memory mapping");
        return false;
    }

    //Copy data
    pe_copy_mem(&pe, 0, pe.image_size, image_mem);
    log_debug("Copied image memory to mapping at %p - %p", image_mem, image_mem + pe.image_size);

    //Resolve imports
    for(int i = 0; i < pe.num_import_libs; i++) {
        struct pe_import_lib *lib = &pe.import_libs[i];
        for(int j = 0; j < lib->num_imports; j++) {
            struct pe_import *imp = &lib->imports[j];

            //Resolve the import
            void *resolv_addr;
            if(imp->ord >= 0) {
                log_warn("DLL %s has ordinal import %s#%d!", name, lib->name, imp->ord);
                resolv_addr = &ord_stub;
            } else {
                resolv_addr = resolve_import(lib->name, imp->name);
            }

            //Write the address into the image
            if(!pe.is_pe32_plus) {
                *((uint32_t*) (image_mem + imp->addr_off)) = (uint32_t) (uint64_t) resolv_addr;
            } else {
                *((uint64_t*) (image_mem + imp->addr_off)) = (uint64_t) (uint64_t) resolv_addr;
            }
        }
    }

    //Apply relocations
    for(int i = 0; i < pe.num_relocs; i++) {
        struct pe_reloc *reloc = &pe.relocations[i];
        if(reloc->addr_bits == 32) {
            *((uint32_t*) (image_mem + reloc->offset)) = (uint32_t) ((uint64_t) image_mem + reloc->delta);
        } else if(reloc->addr_bits == 64) {
            *((uint64_t*) (image_mem + reloc->offset)) = (uint64_t) ((uint64_t) image_mem + reloc->delta);
        } else {
            log_error("Unsupported number of relocation bits! [%d]", reloc->addr_bits);
            return false;
        }
    }
    log_debug("Applied %d relocations", pe.num_relocs);

    //Patch CFG dispatch for indirect call logging
    patch_cfg_dispatch(image_mem, pe.image_size);

    //Apply section protections
    log_debug("Applying memory protections to image");

    if(mprotect(image_mem, pe.image_size, PROT_NONE)) {
        perror("Could't apply default image protection");
        return false;
    }

    for(int i = 0; i < pe.num_sects; i++) {
        struct pe_section *sec = &pe.sections[i];

        int prot = 0;
        if(sec->flags & PE_SECTION_CAN_READ) prot |= PROT_READ;
        if(sec->flags & PE_SECTION_CAN_WRITE) prot |= PROT_WRITE;
        if(sec->flags & PE_SECTION_CAN_EXECUTE) prot |= PROT_EXEC;
        if(mprotect(image_mem + sec->mem_off, sec->mem_size, prot)) {
            perror("Could't apply image section protection");
            return false;
        }

        log_debug("-> section %10s | %p - %p | %c%c%c", sec->name, image_mem + sec->mem_off, image_mem + sec->mem_off + sec->mem_size, (prot & PROT_READ) ? 'r' : '-', (prot & PROT_WRITE) ? 'w' : '-', (prot & PROT_EXEC) ? 'x' : '-');
    }

    //Initialize DLL structure
    dll->base_addr = image_mem;
    dll->image_size = pe.image_size;

    dll->entry_point = pe.entry_point_off ? image_mem + pe.entry_point_off : NULL;

    dll->num_exports = pe.num_exports;
    dll->exports = (struct dll_export*) malloc(pe.num_exports * sizeof(struct dll_export));
    for(int i = 0; i < pe.num_exports; i++) {
        struct pe_export *pexp = &pe.exports[i];
        struct dll_export *dexp = &dll->exports[i];

        dexp->name = strdup(pexp->name);
        dexp->addr = image_mem + pexp->offset;
    }

    //Cleanup
    pe_destroy(&pe);

    return true;
}

void destroy_dll(struct dll_image *dll) {
    //Free allocated memory
    for(int i = 0; i < dll->num_exports; i++) free(dll->exports[i].name);
    free(dll->exports);
    dll->exports = NULL;

    munmap(dll->base_addr, dll->image_size);
    dll->base_addr = NULL;
}

void *find_dll_export(struct dll_image *dll, const char *name) {
    for(int i = 0; i < dll->num_exports; i++) {
        if(strcmp(dll->exports[i].name, name) == 0) return dll->exports[i].addr;
    }

    log_error("Couldn't find DLL export '%s'!", name);
    abort();
}