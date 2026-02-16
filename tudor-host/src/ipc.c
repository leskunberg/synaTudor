#include <unistd.h>
#include <sys/socket.h>
#include <tudor/log.h>
#include "ipc.h"
#include "handler.h"

enum ipc_msg_type ipc_peek_msg(int sock) {
    enum ipc_msg_type msg_type;

    struct iovec iov = {
        .iov_base = &msg_type,
        .iov_len = sizeof(msg_type)
    };

    struct msghdr msg_hdr = {
        .msg_name = NULL,
        .msg_namelen = 0,
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = NULL,
        .msg_controllen = 0
    };

    cant_fail(recvmsg(sock, &msg_hdr, MSG_PEEK));

    return msg_type;
}

size_t ipc_recv_msg(int sock, void *buf, enum ipc_msg_type type, size_t min_sz, size_t max_sz, int *fd, int *fd2) {
    struct iovec iov = {
        .iov_base = buf,
        .iov_len = max_sz
    };

    struct {
        struct cmsghdr hdr;
        int fds[2];
    } cmsg = {
        .hdr.cmsg_len = CMSG_LEN(sizeof(int) * 2),
        .hdr.cmsg_level = SOL_SOCKET,
        .hdr.cmsg_type = SCM_RIGHTS,
        .fds = { -1, -1 }
    };

    struct msghdr msg_hdr = {
        .msg_name = NULL,
        .msg_namelen = 0,
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = &cmsg,
        .msg_controllen = sizeof(cmsg)
    };

    ssize_t msg_size;
    cant_fail(msg_size = recvmsg(sock, &msg_hdr, 0));

    if(msg_size < min_sz || max_sz < msg_size) {
        log_error("Invalid IPC message size: 0x%lx (min 0x%lx, max 0x%lx)", msg_size, min_sz, max_sz);
        abort();
    }

    if(*((enum ipc_msg_type*) buf) != type) {
        log_error("Unexpected IPC message type: 0x%x (expected 0x%x)", *((enum ipc_msg_type*) buf), type);
        abort();
    }

    //Transfer FD(s)
    if(msg_hdr.msg_controllen > 0) {
        if(cmsg.hdr.cmsg_level != SOL_SOCKET || cmsg.hdr.cmsg_type != SCM_RIGHTS) {
            log_error("Invalid IPC control message");
            abort();
        }

        //Determine how many fds were received
        size_t fd_bytes = cmsg.hdr.cmsg_len - CMSG_LEN(0);
        int num_fds = (int)(fd_bytes / sizeof(int));

        if(num_fds >= 1) {
            if(fd) *fd = cmsg.fds[0];
            else cant_fail(close(cmsg.fds[0]));
        } else {
            if(fd) *fd = -1;
        }
        if(num_fds >= 2) {
            if(fd2) *fd2 = cmsg.fds[1];
            else cant_fail(close(cmsg.fds[1]));
        } else {
            if(fd2) *fd2 = -1;
        }
    } else {
        if(fd) *fd = -1;
        if(fd2) *fd2 = -1;
    }

    return msg_size;
}

void ipc_send_msg(int sock, void *buf, size_t size) {
    struct iovec iov = {
        .iov_base = buf,
        .iov_len = size
    };

    struct msghdr msg_hdr = {
        .msg_name = NULL,
        .msg_namelen = 0,
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = NULL,
        .msg_controllen = 0
    };

    cant_fail(sendmsg(sock, &msg_hdr, 0));
}
