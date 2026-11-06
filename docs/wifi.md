# vnc-bridge-wifi

Prints an i3blocks block with the signal strength of a Wi-Fi interface.

```sh
make build
build/bin/vnc-bridge-wifi
```

The interface is taken from `INTERFACE`, then `BLOCK_INSTANCE`; when both are
unset or empty, the first interface listed by `iw dev` is used. Nothing is
printed when no interface is found, when it has no
`/sys/class/net/<interface>/wireless` directory, or when `iw` fails.

The signal reported by `iw dev <interface> link` is converted to a percentage
with `(dBm + 100) * 2`, rounded and limited to 0-100. The output is the
i3blocks three-line format: full text, short text, color. The icon has three
levels (up to 33%, up to 66%, above) and a disconnected link prints `--%`.

| Variable | Used for | Default |
| --- | --- | --- |
| `COLOR_GE80` | 80% and above | `#859900` |
| `COLOR_GE60` | 60% to 79% | `#b58900` |
| `COLOR_GE40` | 40% to 59% | `#cb4b16` |
| `COLOR_LOWR` | below 40% | `#dc322f` |
| `COLOR_DOWN` | no signal | `#586e75` |

`vnc-bridge-wifi --help` prints the usage line and `--version` the
version.

The program is derived from the i3blocks `wifi` script (Copyright 2014
Alexander Keller) and is licensed GPL-3.0-or-later.
