#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/socket.h>

#include <tudor/log.h>
#include <tudor/libcrypto.h>
#include <tudor/tudor.h>
#include "sandbox.h"
#include "ipc.h"
#include "handler.h"

/* From fileops.c — image channel fd */
extern int win_hidraw_fd_img;
extern volatile bool tudor_shutting_down;

static void recv_init_msg(int sock, int *cmd_fd, int *img_fd) {
    struct ipc_msg_init init_msg;
    ipc_recv_msg(sock, &init_msg, IPC_MSG_INIT, sizeof(init_msg), sizeof(init_msg), cmd_fd, img_fd);

    LOG_LEVEL = init_msg.log_level;

    if(!init_msg.has_image_channel && img_fd) {
        /* Only 1 fd was sent; img_fd might be -1 already from ipc_recv_msg */
        if(*img_fd >= 0) {
            close(*img_fd);
            *img_fd = -1;
        }
    }
}

static bool has_sensor_name;
static int pdata_ipc_sock;

//Cached pairing data — avoids redundant IPC round-trips when the DLL
//requests pairing data multiple times (e.g. during initialization).
static const struct tudor_pair_data *cached_pdata = NULL;

static const struct tudor_pair_data *get_pdata_cb(const char *name) {
    log_info("Getting pairing data for sensor '%s'...", name);

    if(has_sensor_name && strcmp(name, probe_sensor_name) != 0){
        log_error("Attempted multiple different sensor pairing data loads!");
        abort();
    } else if(!has_sensor_name) {
        strncpy(probe_sensor_name, name, IPC_SENSOR_NAME_SIZE);
        has_sensor_name = true;
    }

    //Return cached copy if available
    if(cached_pdata) {
        log_info("Returning cached pairing data (%lu bytes)", cached_pdata->data_size);
        return cached_pdata;
    }

    //Send an IPC message to the module
    struct ipc_msg_load_pdata msg = { .type = IPC_MSG_LOAD_PDATA, .sensor_name = {0} };
    strncpy(msg.sensor_name, name, IPC_SENSOR_NAME_SIZE);
    ipc_send_msg(pdata_ipc_sock, &msg, sizeof(msg));

    //Receive the response
    struct {
        struct ipc_msg_resp_load_pdata msg;
        char buf[IPC_MAX_PDATA_SIZE];
    } resp;
    size_t pdata_sz = ipc_recv_msg(pdata_ipc_sock, &resp, IPC_MSG_RESP_LOAD_PDATA, sizeof(resp.msg), sizeof(resp), NULL, NULL) - sizeof(resp.msg);

    //Leak the pairing data buffer ¯\_(ツ)_/¯
    struct tudor_pair_data *pdata = (struct tudor_pair_data*) malloc(sizeof(struct tudor_pair_data) + pdata_sz);
    if(!pdata) {
        perror("Couldn't allocate pairing data buffer");
        abort();
    }
    pdata->data = pdata+1;
    pdata->data_size = pdata_sz;
    memcpy(pdata->data, resp.msg.pdata, pdata->data_size);

    //Cache for future calls
    cached_pdata = pdata;

    return pdata;
}

static void set_pdata_cb(const char *name, const struct tudor_pair_data *data) {
    if(data->data_size > IPC_MAX_PDATA_SIZE) {
        log_error("Pairing data over maximum size!");
        abort();
    }
    log_info("Setting pairing data for sensor '%s'...", name);

    //Record the sensor name (may be the first time we see it if no prior pdata existed)
    if(!has_sensor_name) {
        strncpy(probe_sensor_name, name, IPC_SENSOR_NAME_SIZE);
        has_sensor_name = true;
    }

    //Update cache with new pairing data
    struct tudor_pair_data *pdata = (struct tudor_pair_data*) malloc(sizeof(struct tudor_pair_data) + data->data_size);
    if(pdata) {
        pdata->data = pdata+1;
        pdata->data_size = data->data_size;
        memcpy(pdata->data, data->data, data->data_size);
        cached_pdata = pdata;
    }

    //Send an IPC message to the module
    struct {
        struct ipc_msg_store_pdata msg;
        char buf[IPC_MAX_PDATA_SIZE];
    } msg = { .msg.type = IPC_MSG_STORE_PDATA, .msg.sensor_name = {0} };
    strncpy(msg.msg.sensor_name, name, IPC_SENSOR_NAME_SIZE);
    memcpy(msg.msg.pdata, data->data, data->data_size);
    ipc_send_msg(pdata_ipc_sock, &msg, sizeof(msg.msg) + data->data_size);

    //Wait for ACK
    enum ipc_msg_type resp;
    ipc_recv_msg(pdata_ipc_sock, &resp, IPC_MSG_ACK, sizeof(resp), sizeof(resp), NULL, NULL);
}

int main() {
    //Configure stdout/stderr buffering
    cant_fail(setvbuf(stdout, NULL, _IOLBF, 1024));
    cant_fail(setvbuf(stderr, NULL, _IOLBF, 1024));

    //Check if stdin is a UNIX socket
    struct stat stdin_stat;
    cant_fail(fstat(STDIN_FILENO, &stdin_stat));
    if(!S_ISSOCK(stdin_stat.st_mode)) {
        fputs("This program isn't intended to be executed directly.\n", stderr);
        return EXIT_FAILURE;
    }

    int stdin_dom;
    socklen_t stdin_dom_sz = sizeof(stdin_dom);
    cant_fail(getsockopt(STDIN_FILENO, SOL_SOCKET, SO_DOMAIN, &stdin_dom, &stdin_dom_sz));
    if(stdin_dom != AF_UNIX) {
        log_error("The given socket isn't a UNIX socket!");
        return EXIT_FAILURE;
    }
    int sock = STDIN_FILENO;

    //Pre-load libgcc_s.so.1 before sandboxing — needed by pthread_exit
    //for stack unwinding during DLL thread cleanup.
    dlopen("libgcc_s.so.1", RTLD_NOW | RTLD_GLOBAL);

    //Activate sandbox
    activate_sandbox();
    log_info("Activated sandbox");

    //Receive the init message with hidraw fd(s)
    int cmd_fd = -1, img_fd = -1;
    recv_init_msg(sock, &cmd_fd, &img_fd);
    log_info("Received init message - cmd_fd=%d img_fd=%d", cmd_fd, img_fd);

    //Set the image channel fd globally before tudor_open
    win_hidraw_fd_img = img_fd;

    //Initialize libcrypto
    ERR_load_crypto_strings();
    OpenSSL_add_all_algorithms();
    log_info("Initialized libcrypto");

    //Initialize driver
    tudor_get_pdata_fnc = get_pdata_cb;
    tudor_set_pdata_fnc = set_pdata_cb;
    pdata_ipc_sock = sock;
    if(!tudor_init()) {
        log_error("Couldn't initialize tudor driver!");
        return EXIT_FAILURE;
    }
    log_info("Initialized tudor driver");

    //Open device using the hidraw command channel fd
    struct tudor_device dev;
    struct tudor_device_state state;
    if(!tudor_open(&dev, cmd_fd, &state)) {
        log_error("Couldn't open tudor device!");
        return EXIT_FAILURE;
    }
    log_info("Opened tudor device");

    //Use a default sensor name if the DLL didn't provide one via pairing data callbacks
    if(!has_sensor_name) {
        log_info("DLL did not provide sensor name via pairing data, using default");
        strncpy(probe_sensor_name, "Tudor Sensor", IPC_SENSOR_NAME_SIZE);
        has_sensor_name = true;
    }

    //Send ready message
    enum ipc_msg_type ready_type = IPC_MSG_READY;
    ipc_send_msg(sock, &ready_type, sizeof(ready_type));
    log_info("Sent ready message");

    //Enter handler loop
    run_handler_loop(&dev, sock);

    //Close device (pipeline teardown) — fds still valid so DLL teardown works
    if(!tudor_close(&dev)) {
        log_error("Couldn't close tudor device!");
    }
    log_info("Closed tudor device");

    //Signal shutdown and close fds
    tudor_shutting_down = true;

    if(cmd_fd >= 0) close(cmd_fd);
    if(img_fd >= 0 && img_fd != cmd_fd) close(img_fd);

    //Shutdown tudor driver
    if(!tudor_shutdown()) {
        log_error("Couldn't shutdown tudor driver!");
    }
    log_info("Shutdown tudor driver");

    _exit(EXIT_SUCCESS);
}
