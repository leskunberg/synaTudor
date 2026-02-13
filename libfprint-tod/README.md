# libfprint-tod
This folder contains the libfprint TOD (Third-party Object Driver) module which
integrates with the rest of the system via fprintd. It communicates with the
[tudor-host](../tudor-host/README.md) and
[tudor-host-launcher](../tudor-host-launcher/README.md), and sends IPC commands
to the former to fulfill the user's requests.

The module registers as a udev-backed hidraw device driver, matching the Lenovo
keyboard's VID:PID (17EF:613E for Bluetooth, 17EF:6142 for USB). During probe,
it filters by HID report descriptor to claim only the fingerprint command
channel hidraw device, ignoring the keyboard/touchpad/image interfaces.

It opens the command and image channel hidraw devices and passes them to the
tudor-host process via file descriptor passing (SCM_RIGHTS).

## Documentation
**TODO**
