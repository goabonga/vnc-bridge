# vnc-bridge-pcsc

Lists PC/SC channel sockets and runs a program with `PCSCLITE_CSOCK_NAME`
pointing at one of them, so that the program's `libpcsclite` talks to that
socket instead of the local `pcscd`.

```sh
make build
build/bin/vnc-bridge-pcsc status
build/bin/vnc-bridge-pcsc run -- pcsc_scan
build/bin/vnc-bridge-pcsc run --client PID:FD -- program arguments
build/bin/vnc-bridge-pcsc run --socket /private/dir/PID-FD.sock -- program
```

## Channels

A channel is a socket named `PID-FD.sock` in `<runtime-dir>/pcsc/`, where the
runtime directory is `--runtime-dir` or `$XDG_RUNTIME_DIR/vnc-bridge`. Its
identifier is `PID:FD`. A channel is listed and accepted only when:

- its process `PID` is running;
- it is a socket reached without a symbolic link, owned by the current user and
  not accessible to others;
- its directory is owned by the current user and not accessible to others.

## Commands

`status` prints one `PID:FD<TAB>path` line per channel and exits with status 0, or
1 when there is none.

`run` selects one channel and executes the program directly, without a shell, with
`PCSCLITE_CSOCK_NAME` set to the socket path.

| Option | Effect |
| --- | --- |
| `--runtime-dir DIR` | Look for channels in `DIR/pcsc` |
| `--client PID:FD` | Select the channel with this identifier |
| `--socket PATH` | Use this socket directly; incompatible with `--runtime-dir` and `--client` |

When several channels exist, `--client` is required. The exit status is the one of
the program; it is 1 when no single channel can be selected, 2 for an invalid
command line (including an unset `XDG_RUNTIME_DIR` without `--runtime-dir`), 127 when
the program does not exist and 126 when it cannot be executed.

`vnc-bridge-pcsc --help` prints the usage and `--version` the version.
