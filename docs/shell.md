# vnc-bridge-shell

Connects the current terminal to a shell channel: a Unix stream socket served by
a peer that runs the shell and speaks the frame protocol below.

```sh
make build
build/bin/vnc-bridge-shell --connect --socket /path/to/channel.sock
```

`--connect` and `--socket PATH` are both required (otherwise the exit status is 2).
The program connects with a 3-second limit and refuses a peer which runs as
another user. Messages are in French.

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
It is 1, after printing `Shell du GPD : connexion fermée ou canal terminal invalide`,
when the channel cannot be reached, closes without an `E` frame, or sends an
oversized, unknown or malformed frame. An `E` frame holding 125 means a terminal is
already open: the program prints `Un terminal est déjà ouvert pour ce viewer` and
exits with status 1.

`vnc-bridge-shell --help` prints the usage and `--version` the version.
