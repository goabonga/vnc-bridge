# vnc-bridge-telemetry

Reads the client measurements stored in `status.json` and prints a battery or
volume block.

```sh
make build
build/bin/vnc-bridge-telemetry battery --runtime-dir /path/to/dir
build/bin/vnc-bridge-telemetry audio --hostname client --i3blocks
build/bin/vnc-bridge-telemetry status --runtime-dir /path/to/dir
```

## Commands

| Command | Output |
| --- | --- |
| `battery` | `BAT 73% (host)`, with `+` after the percentage while charging |
| `audio` | `VOL 65%`, or `VOL MUET` when muted |
| `status` | The content of `status.json`, unchanged |

## Options

| Option | Effect |
| --- | --- |
| `--runtime-dir DIR` | Directory holding `status.json`. Default: `$XDG_RUNTIME_DIR/vnc-bridge` |
| `--hostname HOST` | Only consider records of the client with this hostname |
| `--client PID:FD` | Only consider the record with this identifier |
| `--hide-unavailable` | Print an empty line instead of `n/a` when there is no measurement |
| `--i3blocks` | Print Font Awesome icons instead of text |
| `--help` | Print the usage line |
| `--version` | Print the version |

Without `--runtime-dir`, an unset `XDG_RUNTIME_DIR` is an error (exit status 2),
except with `--hide-unavailable` which prints an empty line.

## Record selection

`status.json` holds `{"version":1,"clients":[...]}`. A record is used when its
`received_at` timestamp (seconds since the epoch) is less than 90 seconds old and
it matches `--hostname` and `--client`. The most recently received record wins.

When no record is usable, or the value is null, the output is `BAT n/a` or
`VOL n/a` (`n/a ` with `--i3blocks`, an empty line with `--hide-unavailable`).

## i3blocks output

`battery --i3blocks` prints one line, ` 73% <icon> `, followed by a bolt icon
while charging. The battery icon changes at 10, 35, 65 and 90 percent.

`audio --i3blocks` prints the i3blocks three-line format: full text, short
text and color. The speaker icon has three levels (up to 25%, up to 65%, above)
and a muted volume uses the lowest icon with the color `#586e75`; otherwise the
color is `#fc9600`.

## Exit status

`0` on success, `1` when `status.json` cannot be read by `status`, `2` for an
invalid command line.
