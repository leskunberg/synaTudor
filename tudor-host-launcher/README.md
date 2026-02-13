# tudor-host-launcher
This folder contains a simple D-Bus service capable of launching and managing
tudor_host processes. This is necessary because fprintd is sandboxed in such a
way that the host's own sandbox fails to properly initialize. Because the host
launcher is outside of this sandbox (it does still employ systemd unit
sandboxing, but only to the extent possible when maintaining the host's
functionality), it can properly launch these host processes, and because it
provides its services using D-Bus, the libfprint-tod module can interact with
it and take over IPC once the process has been started.

Host processes are identified by their hidraw device path. The launcher supports
launching, killing, orphaning (keeping alive across libfprint close/reopen
cycles), and adopting orphaned processes.

## Documentation
**TODO**
