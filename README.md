# vnc-bridge

[![ci](https://github.com/goabonga/vnc-bridge/actions/workflows/ci.yml/badge.svg)](https://github.com/goabonga/vnc-bridge/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/goabonga/vnc-bridge)](https://github.com/goabonga/vnc-bridge/releases)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

Native C11 companion utilities for TigerVNC on Arch Linux.

## Build

Building requires `json-c` and `pkgconf`.

```bash
make build      # binaries are written to build/bin/
make install    # under $(PREFIX)/bin, default /usr/local, honouring DESTDIR
```

## Programs

- `vnc-bridge-fido`: lists USB FIDO authenticators and exports one over a
  binary stream, see [docs/fido.md](docs/fido.md).
- `vnc-bridge-i3blocks`: runs `i3blocks` with alternating Solarized
  backgrounds, see [docs/i3blocks.md](docs/i3blocks.md).
- `vnc-bridge-pcsc`: lists PC/SC channel sockets and runs a program using
  one, see [docs/pcsc.md](docs/pcsc.md).
- `vnc-bridge-telemetry`: prints battery and volume blocks from client
  measurements, see [docs/telemetry.md](docs/telemetry.md).
- `vnc-bridge-wifi`: i3blocks block showing Wi-Fi signal strength, see
  [docs/wifi.md](docs/wifi.md).

## Tests

```bash
make test
```

The unit tests run with AddressSanitizer and UndefinedBehaviorSanitizer;
pass `SANITIZE=` to disable them on platforms that reject them.

## Code quality

```bash
make format-check   # clang-format
make lint           # clang-tidy
make format         # apply clang-format
```

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

[MIT](LICENSE). `vnc-bridge-wifi` is GPL-3.0-or-later, see
[LICENSES/GPL-3.0-or-later.txt](LICENSES/GPL-3.0-or-later.txt).
