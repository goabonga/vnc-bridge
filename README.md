# vnc-bridge

[![ci](https://github.com/goabonga/vnc-bridge/actions/workflows/ci.yml/badge.svg)](https://github.com/goabonga/vnc-bridge/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/goabonga/vnc-bridge)](https://github.com/goabonga/vnc-bridge/releases)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

Native C11 companion utilities for TigerVNC on Arch Linux.

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

[MIT](LICENSE)
