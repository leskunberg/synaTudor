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

## Fork Changes
This is a fork of [Popax21/synaTudor](https://github.com/Popax21/synaTudor) with
substantial changes to support the Lenovo X1 Fold 16 keyboard's fingerprint
sensor, which sits behind a HID composite device rather than being a standalone
USB device.

### Transport: libusb to hidraw
The original project used libusb for USB communication. This fork replaces the
entire transport layer with hidraw, which is necessary because the sensor is
exposed as multiple HID interfaces within a composite keyboard device. A new
`hidraw_detect` module in libtudor auto-detects the correct command and image
channel hidraw devices by parsing HID report descriptors and walking sysfs.

### Newer DLL version
The driver DLLs (synaFpAdapter153.dll, synaWudfBioHid153.dll) are bundled
directly rather than downloaded from an installer. This newer DLL version
required extensive new WinAPI stubs (HID, SetupAPI, User32, console, power
management, RPC, security, WTS, and WDF HID support).

### DLL storage mode
Uses the DLL's own `WbioQueryStorageInterface` for template storage — templates
are stored on the sensor itself rather than in host memory. This means
`IdentifyFeatureSet` queries the sensor directly and newly enrolled templates are
immediately available without restarting the DLL.

### fprintd integration
The libfprint-tod plugin, tudor-host, and tudor-host-launcher have all been
updated for hidraw:
- libfprint-tod uses `FPI_DEVICE_UDEV_SUBTYPE_HIDRAW` and sends hidraw fds via
  SCM_RIGHTS
- tudor-host-launcher identifies devices by hidraw path (was USB bus:addr)
- Verification uses identify (biometric search) instead of verify (GUID lookup),
  because the DLL doesn't preserve GUIDs in its storage

### Other notable changes
- Pre-load libgcc_s.so.1 before sandbox activation (needed for pthread_exit
  stack unwinding during DLL thread cleanup)
- Pairing data caching in tudor-host to avoid redundant IPC round-trips
- No device reopen after enrollment (DLL storage mode makes it unnecessary, and
  reopening causes the DLL's TLS session to hang)

## Supported Hardware
Currently targets the Synaptics Tudor fingerprint sensor (06CB:00DD) embedded in
the Lenovo X1 Fold 16 keyboard (VID:PID 17EF:613E over Bluetooth, 17EF:6142
over USB). The sensor communicates via HID over hidraw.

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
The Windows driver DLLs are bundled in `libtudor/dlls/`.

### Dependencies
- meson (>= 0.57.0)
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
