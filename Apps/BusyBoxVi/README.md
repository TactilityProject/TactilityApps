# Vi

The [BusyBox](https://busybox.net) `vi` text editor (version 1.37.0) for the Tactility terminal.

## Usage

Run it from the Terminal app:

```
vi [-c CMD] [-R] [-H] [FILE]...
```

- `-c CMD`: run a colon command at startup
- `-R`: read-only mode
- `-H`: list available features

## Features

- Colon commands (`:w`, `:q`, `:wq`, `:e`, `:r`, `:s/old/new/`, ...)
- Yank/put and marks
- Search (plain text, no regular expressions)
- `.` repeat, undo, `:set` options (autoindent, ignorecase, showmatch, tabstop)

Not supported: regular expressions, `:!` shell commands, signals (suspend/resize).
The terminal has no Home/End/PgUp/PgDn keys: use `0`, `$`, `Ctrl+B` and `Ctrl+F`.

## Source

- `main/Source/vi.c` and `main/Source/read_key.c`: unmodified from BusyBox 1.37.0 (`editors/vi.c`, `libbb/read_key.c`)
- `main/Include/libbb.h` and `main/Source/libbb.c`: the subset of BusyBox's libbb that these need, plus the feature configuration

## License

Unlike the other apps in this repository, this app is licensed under [GPL v2](LICENSE) (only), as required by BusyBox.
