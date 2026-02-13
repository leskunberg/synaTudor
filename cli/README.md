# CLI Wrapper
This folder contains a simple CLI wrapper for the functionality of the relinked
driver. It can be used to enroll, verify and identify captured samples of fingerprints.

**WARNING:** Even though the CLI employs sandboxing, its security is in no way
comparable to the one found in the libfprint integration. A malicious driver
could take over your local user account! This CLI is only intended to be used
for debugging and/or small scale tests.

## Usage
Start the wrapper using `sudo ./build/cli/tudor_cli <path to data store file> <flags>`.
The data store file is a file where the driver will store data like pairing data
and enrollment records, and has to be in a directory accessible by your own user
(the wrapper drops privileges before opening the file for security reasons).

Currently, the following flags are defined:

Flag | Description
----- | ---------------------------
`-v` | Increase the verbosity of the log output
`-q` | Decrease the verbosity of the log output
`-t` | Enable display of driver debug trace messages
`-H <path>` | Set the hidraw command channel device path (e.g. `/dev/hidraw5`)
`-I <path>` | Set the hidraw image channel device path (e.g. `/dev/hidraw2`)

If `-H` and `-I` are not specified, the CLI auto-detects the correct hidraw
devices by scanning `/sys/class/hidraw/` for the keyboard's VID:PID and
identifying channels by their HID report descriptors.

Once the program is running, after some time, a command prompt should appear.
All available commands are displayed there.

## Documentation
**TODO**
