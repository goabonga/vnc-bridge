# vnc-bridge-fido

Lists USB FIDO authenticators, checks that one answers, and exports one of them
over standard input and output with a small binary protocol.

```sh
make build
build/bin/vnc-bridge-fido list
build/bin/vnc-bridge-fido probe --device /dev/hidraw3
build/bin/vnc-bridge-fido export --device /dev/hidraw3
```

## list

`list` prints one line per accessible FIDO interface:

```
/dev/hidraw3 0483:5741 Name of the device
```

Only hidraw nodes of USB devices whose report descriptor is a single unnumbered
FIDO application collection (usage page `0xF1D0`) with 64-byte input and output
reports are listed. Control characters in the name are replaced with `_`.

## probe

`probe --device /dev/hidrawN` opens the device exclusively and sends a CTAPHID_INIT
request with a random nonce on the broadcast channel. INIT only negotiates a
channel: nothing is registered or signed. When the answer carries the nonce, it
prints

```
CTAPHID_INIT OK; HID framing=2; capabilities=0x05
```

(the values are bytes 19 and 23 of the answer) and exits with status 0. After 3
seconds without a matching answer it prints `FIDO INIT timed out` on standard error
and exits with status 1.

## export

`export --device /dev/hidrawN` opens the device read-write, takes an exclusive
lock on it and relays reports between the device and standard input/output. It
refuses to run when standard input or output is a terminal (exit status 2).
`--identity DEV:INO:RDEV` (decimal numbers) makes it fail with status 1 unless the
opened device is the file with these `st_dev`, `st_ino` and `st_rdev`.

The stream is a sequence of frames: one byte kind, a 16-bit big-endian length and
the payload.

| Kind | Direction | Payload |
| --- | --- | --- |
| `I` | to standard output, once, first | Vendor id and product id, 2 bytes each, big-endian |
| `D` | both ways | A 64-byte report, forwarded unchanged |

Reports written to the device get a leading zero report id. A frame that is not a
64-byte `D` frame, a stream that ends inside a frame, or a device error ends the
program with status 1. End of input on a frame boundary, `SIGTERM`, `SIGINT` and
`SIGHUP` end it with status 0. Report contents are never logged.

`vnc-bridge-fido --help` prints the usage and `--version` the version.
