# Synaptics Tudor Driver Relinking Project
This project attempts to dynamically relink the driver of the Synaptics Tudor
family of fingerprint sensors at run time, allowing them to run and provide
their functionality on Linux-based x86-64 systems. It split off from the reverse
engineering branch after it hit multiple dead ends, and because it showed the
potential of quickly allowing for the creation of *something* which can at least
allow users to use the sensors installed in their hardware.

THIS PROJECT IS PROVIDED AS IS, WITHOUT WARRANTY OR LIABILITY OF ANY KIND, OR
FOR ANY RISKS OR SIDE EFFECTS WHICH MIGHT OCCUR FROM USAGE OF ANYTHING PROVIDED
AS PART OF THIS PROJECT, INCLUDING, BUT NOT LIMITED TO, BRICKED SENSORS,
CORRUPTED FIRMWARE, BYPASSES OF HOST SECURITY, AND VULNERABILITIES IN THE CODE.
USE AT YOUR OWN RISK.

## Supported Hardware
Currently targets the Synaptics Tudor fingerprint sensor (06CB:00DD) embedded in
the Lenovo X1 Fold 16 keyboard (VID:PID 17EF:613E over Bluetooth, 17EF:6142
over USB). The sensor communicates via HID over hidraw.

**USB mode is required** — the sensor needs both a command channel and an image
channel (separate HID interfaces), and the image channel is only available over
USB. Bluetooth mode exposes only a single hidraw device without the image
channel, which is insufficient for fingerprint capture.

## Structure
This project is split over multiple folders, all providing different parts of
the functionality:
- [libtudor](libtudor/README.md): Contains the common library code handling
  relinking, interfacing with the driver, and hidraw device detection.
- [cli](cli/README.md): Contains a simple CLI wrapper for the relinked driver.
- [tudor-host](tudor-host/README.md): Contains the sandboxed host process for
  libtudor, used by the libfprint module. Receives hidraw file descriptors over
  IPC from the libfprint module.
- [tudor-host-launcher](tudor-host-launcher/README.md): Contains the systemd
  D-Bus service which launches and manages tudor host processes. This extra step
  is needed to bypass the strict fprintd sandboxing, which interferes with the
  host's own stricter sandboxing.
- [libfprint-tod](libfprint-tod/README.md): Contains the libfprint TOD module,
  which integrates with fprintd for system-level fingerprint authentication
  (login, sudo, screen unlock).

## Building / Installation
The same build system used by libfprint, meson, is used for this project.
During the first build, the Windows driver is automatically downloaded and
extracted. `innoextract` has to be installed for this.

### Dependencies
- meson (>= 0.57.0)
- innoextract (for driver extraction)
- libcrypto (OpenSSL)
- libcap
- libseccomp
- glib/gio (>= 2.0)
- dbus-1
- json-glib-1.0
- libfprint-tod (for the fprintd integration module)

### Build
```sh
meson setup build
ninja -C build
sudo ninja -C build install
```
(for Arch Linux specifically, you might want to use `arch-meson` instead of `meson setup`)

For documentation about build options etc., see the individual parts.

For the libfprint module to be picked up and work, you'll need to have a
`libfprint-tod` fork of libfprint installed. Most Linux distributions have a
separate package which you can install instead of the regular libfprint one
(e.g. Arch Linux: AUR `libfprint-tod-git`).

### Quick Start (CLI)
The CLI can be used for standalone testing without fprintd:
```sh
sudo ./build/cli/tudor_cli /tmp/tudor_data -vv
```
This auto-detects the hidraw devices. To specify them manually:
```sh
sudo ./build/cli/tudor_cli /tmp/tudor_data -vv -H /dev/hidraw5 -I /dev/hidraw2
```
Where `-H` is the command channel and `-I` is the image channel.

### Quick Start (fprintd)
After installing, enable and start the host launcher service:
```sh
sudo systemctl daemon-reload
sudo systemctl enable --now tudor-host-launcher
```
Then use fprintd as usual:
```sh
fprintd-list           # verify device is detected
fprintd-enroll         # enroll a fingerprint
fprintd-verify         # verify against enrolled fingerprint
```
