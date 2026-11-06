# vnc-bridge-shell

Connects the current terminal to a shell channel: a Unix stream socket served by
a peer that runs the shell and speaks the frame protocol below.

```sh
make build
build/bin/vnc-bridge-shell --connect --socket /path/to/channel.sock
build/bin/vnc-bridge-shell --connect --runtime-dir /path/to/dir --hostname nano
```

With `--connect`, the program connects with a 3-second limit and refuses a peer
which runs as another user. Messages are in English.

## Without --connect

Without `--connect` the program opens a terminal window titled `GPD · local RFB shell`
that runs itself with `--connect` and the same options:

```sh
build/bin/vnc-bridge-shell
build/bin/vnc-bridge-shell --hostname nano
```

When started without any option, it first asks i3 (`i3-msg`) for a window with that
title and focuses it if there is one, in which case no terminal is opened.

The terminal is the `terminal` entry of the configuration file (a program name or
path, started as `terminal -T TITLE -e PROGRAM ...`), else `alacritty`, else `xterm`.
With none of them, the program prints `Install alacritty or xterm, set
"terminal" in shell.json, or use --connect` and exits with status 2.

## Selecting the channel

`--socket PATH` connects to this socket. Otherwise the program looks for
`<directory>/shell/PID-FD.sock` files and needs exactly one candidate. A candidate is
a socket owned by the current user whose process `PID` is running.

| Option | Effect |
| --- | --- |
| `--runtime-dir DIR` | Directory containing `shell/` and `status.json` |
| `--client PID:FD` | Keep only the socket of this identifier |
| `--hostname HOST` | Keep only sockets whose client has this hostname in `DIR/status.json` |

Without `--runtime-dir`, the directory is the `runtime_dir` of the configuration, else
the directory of `$VNC_BRIDGE_SOCKET` (an absolute path), else
`$XDG_RUNTIME_DIR/vnc-bridge` (`/run/user/UID` when `XDG_RUNTIME_DIR` is unset).
Without `--hostname` and `--client`, the hostname is the `hostname` of the configuration.

The configuration is the optional JSON file `~/.config/vnc-bridge/shell.json`;
a missing or invalid file is ignored.

```json
{"runtime_dir": "/run/user/1000/vnc-bridge", "hostname": "nano", "terminal": "alacritty"}
```

With zero or several candidates the program prints
`No RFB channel from the GPD available. Enable VNC_BRIDGE_SHELL=1 in the viewer and reconnect.`
or `Several viewers available; specify --client PID:FD`, then the failure message
below, and exits with status 1.

## Terminal handling

When standard input is a terminal, it is switched to raw mode (no echo, no
translation) and restored on exit. The window size is sent when connecting and
again after each `SIGWINCH`. When standard input is not a terminal the size sent
is 24 rows by 80 columns. Standard output is made non-blocking while the program
runs and restored afterwards.

## Frames

A frame is one byte kind, a 16-bit big-endian length (at most 55) and the payload.

| Kind | Direction | Payload |
| --- | --- | --- |
| `O` | to the peer, first | Rows and columns, 16-bit big-endian each |
| `W` | to the peer, after a resize | Rows and columns, same format |
| `D` | both ways | Bytes typed by the user (at most 55 per frame) / bytes to print |
| `E` | from the peer | One byte: the exit status of the remote shell |

End of standard input is sent as a `D` frame holding the byte `0x04`.

## Exit status

The exit status is the byte of the `E` frame, once all received output has been
written. It is 128 plus the signal number after `SIGINT`, `SIGTERM` or `SIGHUP`.
It is 1, after printing `GPD shell: connection closed or invalid terminal channel`,
when the channel cannot be reached, closes without an `E` frame, or sends an
oversized, unknown or malformed frame. An `E` frame holding 125 means a terminal is
already open: the program prints `A terminal is already open for this viewer` and
exits with status 1.

`vnc-bridge-shell --help` prints the usage and `--version` the version.
