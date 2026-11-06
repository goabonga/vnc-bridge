# vnc-bridge-i3blocks

Runs `i3blocks` and restyles its i3bar JSON stream: visible blocks get
alternating Solarized backgrounds joined by arrow separators.

```sh
make build
build/bin/vnc-bridge-i3blocks -c ~/.config/i3blocks/config
```

Every argument is passed to `i3blocks`. Standard input is inherited, so click
events reach `i3blocks` unchanged, and the exit status of `i3blocks` is the
exit status of the program. The program stops `i3blocks` when it receives
`SIGTERM` or `SIGINT`, or when the stream is malformed (exit status 1).

For each array of blocks:

- blocks named `pad` or `sep`, and blocks whose `full_text` is empty or only
  whitespace, are removed;
- every other block receives `background` `#073642` and `#002b36` in turn,
  `separator` set to `false` and `separator_block_width` set to `0`;
- an arrow block (`name` `__solarized_sep`, glyph U+E0B2) is inserted before
  each block, colored like the block and with the previous background;
- `full_text` and `short_text` get one leading space and one trailing space;
- all other fields (`name`, `instance`, `color`, `urgent`, ...) are preserved.

`vnc-bridge-i3blocks --help` prints the usage line and `--version` the
version; both are only recognised as the first argument.
