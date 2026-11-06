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
| `daemon` | Runs the collector that writes `status.json` |
| `battery` | `BAT 73% (host)`, with `+` after the percentage while charging |
| `audio` | `VOL 65%`, or `VOL MUET` when muted |
| `status` | The content of `status.json`, unchanged |
| `local-battery` | Battery of the local machine, in the `--i3blocks` format |
| `local-audio` | Output volume of the local machine, in the `audio --i3blocks` format |

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

## Collector

`daemon` receives measurements on a Unix datagram socket and keeps `status.json`
up to date. It runs in the foreground until `SIGTERM` or `SIGINT`.

```sh
build/bin/vnc-bridge-telemetry daemon --runtime-dir "$XDG_RUNTIME_DIR/vnc-bridge"
```

The directory (and its parents) is created with mode `0700`. It must belong to
the current user and not be accessible to others, otherwise the collector exits
with status 1. It contains:

| File | Content |
| --- | --- |
| `telemetry.sock` | Datagram socket, mode `0600` |
| `status.json` | Current records, mode `0600`, replaced atomically |
| `collector.lock` | Lock: a second collector for the same directory exits with status 1 |

Each datagram is `PID:CONNECTION|CAPACITY|STATE|HOSTNAME`, optionally followed by
`|VOLUME|MUTE`. It is accepted only when all of these hold:

- the sender is the same user and `PID` is the sender's own process id;
- `PID` is a positive decimal number without leading zero, `CONNECTION` a decimal number;
- `CAPACITY` is 0 to 100, or 255 for unknown (stored as `null`);
- `STATE` is 0 to 4: `Unknown`, `Charging`, `Discharging`, `Full`, `Not charging`;
- `HOSTNAME` has at most 48 characters among letters, digits, `_`, `.` and `-`;
- `VOLUME` is 0 to 1000, or 65535 for unknown (stored as `null`, and then `MUTE` must be `0`);
- `MUTE` is `0` or `1`;
- the datagram has at most 128 bytes and no NUL byte.

Anything else, including file descriptors sent with the datagram, is dropped.
A record is identified by `PID:CONNECTION`; a new datagram with the same identifier
replaces it, a record not refreshed for 90 seconds is removed, and at most 256
records are kept. When the collector stops, it removes the socket and leaves
`status.json` with no clients.

## Local battery

`local-battery` needs no runtime directory and reads
`/sys/class/power_supply`. The first battery in name order that is present
(`present` is not `0`) and has an integer `capacity` from 0 to 100 is printed
in the `battery --i3blocks` format; an empty line is printed when there is none.
A `status` of `Charging` shows the bolt. When the battery reports `Unknown`, the
first `USB` or `Mains` supply with the status `Charging`, `Discharging` or
`Not charging` decides.

## Local audio

`local-audio` needs no runtime directory. It prints the volume block, or an empty
line when no volume can be read within one second. The backend is chosen by
`VNC_BRIDGE_AUDIO_BACKEND` (`pipewire` or `alsa`; any other value prints an empty
line).

| Backend | Command | Notes |
| --- | --- | --- |
| `pipewire` | `wpctl get-volume @DEFAULT_AUDIO_SINK@` | `Volume: 0.38` with an optional `[MUTED]`; levels above 1000% are rejected |
| `alsa` | `amixer -c CARD sget CONTROL` | The loudest `Playback` channel; muted when no channel is `[on]` |
| `alsa` with `mixer` | `mixer --card sysdefault:CARD --get CONTROL` | Reply `<percent\|-> <on\|off\|->`; `amixer` answers when `mixer` is absent or fails |

| Variable | Default |
| --- | --- |
| `VNC_BRIDGE_AUDIO_BACKEND` | `pipewire`, or `alsa` on a GPD Pocket |
| `VNC_BRIDGE_ALSA_CARD` | `default`, or `chtrt5645` on a GPD Pocket |
| `VNC_BRIDGE_ALSA_CONTROL` | `Master`, or `DAC1` on a GPD Pocket |
| `VNC_BRIDGE_ALSA_TOOL` | `mixer` on a GPD Pocket with the card `chtrt5645`, otherwise `amixer`; any value other than `mixer` selects `amixer` |

A GPD Pocket is recognized by `chtrt5645` in `/proc/asound/cards`.

## Exit status

`0` on success, `1` when `status.json` cannot be read by `status`, `2` for an
invalid command line.
