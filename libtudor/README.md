# libtudor
This folder contains the common library which relinks and interfaces with the
native Windows driver. It is used by the [CLI wrapper](../cli/README.md) and the
[libfprint host](../tudor-host/README.md).

It also provides hidraw device detection (`tudor/hidraw_detect.h`), which
identifies the fingerprint sensor's command and image channels by scanning
sysfs and parsing HID report descriptors.

## Driver Download / Extraction
During your first build, the Windows driver will automatically be downloaded and
extracted (you'll need a working internet connection). This requires
`innoextract` to be installed (https://pkgs.org/download/innoextract).

## Build Options
Currently, the following build options are defined:

Flag | Description
----- | ---------------------------
`DBGIMPORT=true` | Enable import debugging, which prints the name of unresolved function imports when they're called
`DBGWDF=true` | Enable WDF debugging, which prints the index of unresolved WDF functions when they're called

## Documentation
**TODO**
