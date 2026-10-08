# Vi

The [neatvi](https://github.com/aligrudi/neatvi) text editor for the Tactility terminal.

## Usage

Run it from the Terminal app:

```
vi [-v] [-e] [FILE]...
```

- `-v`: start in vi mode (default)
- `-e`: start in ex mode

## Features

- vi and ex commands, including `:w`, `:q`, `:wq`, `:e`, `:r`, `:s/old/new/`
- Regular expression search and substitution
- Multiple buffers, registers, marks, undo/redo
- Syntax highlighting (see `conf.c`)

Not supported: `:!` shell commands, filters and pipes, and LSP support (Tactility can't start other processes).
The terminal has no Home/End/PgUp/PgDn keys: use `0`, `$`, `Ctrl+B` and `Ctrl+F`.

## Source

`main/Source` holds neatvi (upstream commit `26e9cad`), unmodified, without `cmd.c`, `lsp.c`, `json.c` and `stag.c`.
`main/Source/tactility.c` replaces `cmd.c` and `lsp.c`, which need `fork()`.

## License

neatvi is licensed under the ISC license, see [LICENSE](LICENSE).
