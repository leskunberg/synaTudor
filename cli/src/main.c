#include <sys/types.h>
#include <sys/syscall.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <ucontext.h>
#include <execinfo.h>

#include <tudor/log.h>
#include <tudor/libcrypto.h>
#include <tudor/tudor.h>
#include "datastore.h"
#include "cli.h"
#include "hidraw_detect.h"

/* From fileops.c — image channel fd and shutdown flag */
extern int win_hidraw_fd_img;
extern volatile bool tudor_shutting_down;

static void segfault_handler(int sig, siginfo_t *info, void *ucontext) {
    ucontext_t *uc = (ucontext_t *)ucontext;
    void *pc = (void *)uc->uc_mcontext.gregs[REG_RIP];
    pid_t tid = (pid_t)syscall(SYS_gettid);
    pid_t pid = getpid();
    fprintf(stderr, "\n[CRASH] SIGSEGV at PC=%p, fault addr=%p (pid=%d, tid=%d, %s)\n",
        pc, info->si_addr, pid, tid, (tid == pid) ? "MAIN THREAD" : "BACKGROUND THREAD");

    /* Print some register context */
    fprintf(stderr, "[CRASH] RAX=%016lx RBX=%016lx RCX=%016lx RDX=%016lx\n",
        (unsigned long)uc->uc_mcontext.gregs[REG_RAX],
        (unsigned long)uc->uc_mcontext.gregs[REG_RBX],
        (unsigned long)uc->uc_mcontext.gregs[REG_RCX],
        (unsigned long)uc->uc_mcontext.gregs[REG_RDX]);
    fprintf(stderr, "[CRASH] RSI=%016lx RDI=%016lx RSP=%016lx RBP=%016lx\n",
        (unsigned long)uc->uc_mcontext.gregs[REG_RSI],
        (unsigned long)uc->uc_mcontext.gregs[REG_RDI],
        (unsigned long)uc->uc_mcontext.gregs[REG_RSP],
        (unsigned long)uc->uc_mcontext.gregs[REG_RBP]);
    fprintf(stderr, "[CRASH] R8 =%016lx R9 =%016lx R10=%016lx R11=%016lx\n",
        (unsigned long)uc->uc_mcontext.gregs[REG_R8],
        (unsigned long)uc->uc_mcontext.gregs[REG_R9],
        (unsigned long)uc->uc_mcontext.gregs[REG_R10],
        (unsigned long)uc->uc_mcontext.gregs[REG_R11]);

    /* Print backtrace */
    fprintf(stderr, "[CRASH] Backtrace:\n");
    void *bt[32];
    int bt_size = backtrace(bt, 32);
    /* Replace first frame with actual crash PC */
    if (bt_size > 0) bt[0] = pc;
    char **bt_syms = backtrace_symbols(bt, bt_size);
    for (int i = 0; i < bt_size; i++) {
        fprintf(stderr, "  [%d] %s\n", i, bt_syms ? bt_syms[i] : "???");
    }
    if (bt_syms) free(bt_syms);

    /* Print /proc/self/maps for address mapping */
    fprintf(stderr, "[CRASH] Memory maps around PC and backtrace:\n");
    FILE *f = fopen("/proc/self/maps", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            unsigned long start, end;
            if (sscanf(line, "%lx-%lx", &start, &end) == 2) {
                if ((unsigned long)pc >= start && (unsigned long)pc < end) {
                    fprintf(stderr, "  >>> %s", line);
                } else if (info->si_addr && (unsigned long)info->si_addr >= start && (unsigned long)info->si_addr < end) {
                    fprintf(stderr, "  *>> %s", line);
                }
                /* Also check backtrace frames */
                for (int i = 1; i < bt_size; i++) {
                    if ((unsigned long)bt[i] >= start && (unsigned long)bt[i] < end) {
                        fprintf(stderr, "  [%d] %s", i, line);
                        break;
                    }
                }
            }
        }
        fclose(f);
    }

    _exit(139);
}

static bool drop_root_priv() {
    if(geteuid() == 0 || getegid() == 0) {
        //Determine UID and GID to drop to
        uid_t uid, euid, nuid = getuid();
        if(nuid == 0) getresuid(&uid, &euid, &nuid);
        if(nuid == 0 && secure_getenv("SUDO_UID")) nuid = (uid_t) strtol(secure_getenv("SUDO_UID"), NULL, 0);

        gid_t gid, egid, ngid = getgid();
        if(ngid == 0) getresgid(&gid, &egid, &ngid);
        if(ngid == 0 && secure_getenv("SUDO_GID")) ngid = (uid_t) strtol(secure_getenv("SUDO_GID"), NULL, 0);

        if(nuid == 0 || ngid == 0) {
            log_error("Running as root and not able to determine UID/GID to switch to!");
            return false;
        }

        //Drop privileges
        log_info("Dropping root privileges... [new uid=%d new gid=%d]", nuid, ngid);
        cant_fail(setresgid(ngid, ngid, ngid));
        cant_fail(setresuid(nuid, nuid, nuid));

        //Be paranoid and really make sure that we dropped privileges
        if(geteuid() == 0 || getegid() == 0) {
            log_error("Somehow wasn't able to drop root privileges!");
            return false;
        }

        if(setuid(0) == 0 || seteuid(0) == 0|| setgid(0) == 0 || setegid(0) == 0) {
            log_error("Was somehow able to regain root privileges!");
            return false;
        }
    }
    return true;
}

int main(int argc, char **argv) {
    //Parse arguments
    if(argc < 2) {
        log_error("Usage: %s <data store> [flags]", argv[0]);
        log_error("Flags: -v verbose  -q quiet  -t traces  -H <hidraw_cmd>  -I <hidraw_img>");
        return EXIT_FAILURE;
    }

    const char *hidraw_path = NULL;
    const char *hidraw_img_path = NULL;
    for(int i = 2; i < argc; i++) {
        for(char *p = argv[i]+1; *p; p++) {
            if(*p == 'v' && LOG_LEVEL > LOG_VERBOSE) LOG_LEVEL--;
            if(*p == 'q' && LOG_LEVEL < LOG_ERROR) LOG_LEVEL++;
            if(*p == 't') tudor_log_traces = true;
            if(*p == 'H') { hidraw_path = p+1; if(!*hidraw_path && i+1 < argc) hidraw_path = argv[++i]; break; }
            if(*p == 'I') { hidraw_img_path = p+1; if(!*hidraw_img_path && i+1 < argc) hidraw_img_path = argv[++i]; break; }
        }
    }

    //Auto-detect hidraw devices if not specified on command line
    struct hidraw_detect_result detect = {0};
    if(!hidraw_path) {
        if(hidraw_autodetect(&detect)) {
            hidraw_path = detect.cmd_path;
            if(!hidraw_img_path)
                hidraw_img_path = detect.img_path;
            log_info("Auto-detected: cmd=%s img=%s", hidraw_path, hidraw_img_path);
        } else {
            log_error("No fingerprint sensor found! Specify device with -H <hidraw_cmd> [-I <hidraw_img>]");
            return EXIT_FAILURE;
        }
    }
    if(!hidraw_img_path) hidraw_img_path = hidraw_path;

    //Install SIGSEGV handler for debugging
    struct sigaction sa = {0};
    sa.sa_sigaction = segfault_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);

    //Ask if one wants to really use this
    puts(">>>>> WARNING <<<<<");
    puts("Even though the CLI employs sandboxing, its security is in no way comparable to the one found in the libfprint integration.");
    puts("A malicious driver could take over your local user account!");
    puts("This CLI is only intended to be used for debugging and/or small scale tests.");
    printf("Press 'y' to continue, any key to exit: ");
    char chr = 0;
    scanf("%c", &chr);
    if(chr != 'y') {
        puts("Exiting....");
        return EXIT_FAILURE;
    }

    //Initialize libcrypto
    log_info("Initializing libcrypto...");
    ERR_load_crypto_strings();
    OpenSSL_add_all_algorithms();

    //Open the HID command channel device
    log_info("Opening HID command channel %s...", hidraw_path);
    int hidraw_fd = open(hidraw_path, O_RDWR | O_NONBLOCK);
    if(hidraw_fd < 0) {
        log_error("Error opening HID device %s: %s", hidraw_path, strerror(errno));
        return EXIT_FAILURE;
    }
    log_info("Opened HID command channel %s (fd %d)", hidraw_path, hidraw_fd);

    //Open the HID image channel device
    log_info("Opening HID image channel %s...", hidraw_img_path);
    int hidraw_img_fd = open(hidraw_img_path, O_RDWR | O_NONBLOCK);
    if(hidraw_img_fd < 0) {
        log_warn("Could not open HID image channel %s: %s (continuing without it)", hidraw_img_path, strerror(errno));
        hidraw_img_fd = -1;
    } else {
        log_info("Opened HID image channel %s (fd %d)", hidraw_img_path, hidraw_img_fd);
    }
    /* Set the image channel fd globally before tudor_open */
    win_hidraw_fd_img = hidraw_img_fd;

    //Drop root privileges (if we have them)
    if(!drop_root_priv()) return EXIT_FAILURE;

    //Initialize tudor driver
    tudor_get_pdata_fnc = get_pair_data;
    tudor_set_pdata_fnc = set_pair_data;
    log_info("Initializing tudor driver...");
    if(!tudor_init()) {
        log_error("Error initializing tudor driver!");
        return EXIT_FAILURE;
    }

    //Load data from data store
    log_info("Loading data from data store '%s'...", argv[1]);

    FILE *data_store = fopen(argv[1], "r+");
    if(!data_store && access(argv[1], F_OK)) data_store = fopen(argv[1], "w+");
    if(!data_store) {
        perror("Couldn't open data store file");
        return EXIT_FAILURE;
    }

    if(!load_datastore_pair_data(data_store)) {
        log_error("Couldn't load pairing data from data store!");
        return EXIT_FAILURE;
    }

    //Open the device
    log_info("Opening tudor device...");
    struct tudor_device device;
    if(!tudor_open(&device, hidraw_fd, NULL)) {
        log_error("Error opening tudor device!");
        return EXIT_FAILURE;
    }

    //Load records
    log_info("Loading device records...");
    if(!load_datastore_records(data_store, &device)) {
        log_error("Couldn't load records from data store!");
        return EXIT_FAILURE;
    }

    //Main command loop
    cli_main_loop(&device);

    //Save data to store
    log_info("Saving data to data store '%s'...", argv[1]);

    if(ftruncate(fileno(data_store), 0) || fseek(data_store, 0, SEEK_SET)) {
        perror("Error writing to data store");
        return EXIT_FAILURE;
    }

    if(!save_datastore_pair_data(data_store)) {
        log_error("Couldn't save pairing data to data store!");
        return EXIT_FAILURE;
    }
    if(!save_datastore_records(data_store, &device)) {
        log_error("Couldn't save records to data store!");
        return EXIT_FAILURE;
    }

    if(fclose(data_store)) {
        perror("Error closing data store");
        return EXIT_FAILURE;
    }

    //Close the device (pipeline teardown) — fds still valid, shutdown flag NOT set,
    //so DLL teardown reads/writes work normally against the live hidraw device.
    log_info("Closing tudor device...");
    tudor_close(&device);

    //Now signal background threads to stop and close fds
    log_info("Signaling shutdown...");
    tudor_shutting_down = true;

    log_info("Closing hidraw fds...");
    close(hidraw_fd);
    if(hidraw_img_fd >= 0 && hidraw_img_fd != hidraw_fd)
        close(hidraw_img_fd);

    //Brief wait for background threads to notice fd closure
    usleep(100000);

    //Shutdown tudor driver (unloads DLL code)
    log_info("Shutting down tudor driver...");
    tudor_shutdown();
    free_pair_data();

    //Use _exit to terminate immediately — DLL background threads may still be
    //running and would crash if they try to execute unmapped code after return.
    //All data is already saved, so this is safe.
    log_info("Done.");
    _exit(EXIT_SUCCESS);
}
