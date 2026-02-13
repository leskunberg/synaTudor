# tudor-host
This folder contains the Tudor host process. It's launched by the
[tudor-host-launcher](../tudor-host-launcher/README.md), and its job is to
provide [libtudor](../libtudor/README.md)'s functionality over a secured and
sandboxed IPC connection.

The host process receives hidraw file descriptors (command channel and
optionally image channel) from the libfprint module via SCM_RIGHTS over its
IPC socket. It runs in a strict sandbox with seccomp, user namespaces,
capability dropping, and resource limits.

## Build Options
Currently, the following build options are defined:

Flag | Description
----- | ---------------------------
`UNMOUNTFS=true` | Enable unmounting of the root file system in the sandbox, which prevents the driver from accessing any files. Enabled by default, disable when debugging using e.g. GDB.

## Documentation
**TODO**
