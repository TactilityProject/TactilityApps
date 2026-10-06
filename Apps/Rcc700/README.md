# rcc700

[rcc700](https://github.com/valdanylchuk/rcc700) is a self-hosting mini C compiler for ESP32 (RISC-V).
It is a port of [xcc700](https://github.com/valdanylchuk/xcc700).

## Usage

Run it from the Terminal app:

```
rcc700 FILE.c [-o OUTPUT]
```

Without `-o`, the output is named after the input: `something.c` becomes `something.elf`.
The output is a RV32 ELF for the [ESP-IDF elf_loader](https://components.espressif.com/components/espressif/elf_loader/).

It supports a small subset of C: see the [xcc700 README](https://github.com/valdanylchuk/xcc700#what-is-missing).
rcc700 allocates a 256 KiB code buffer, so it requires PSRAM.

## Source

`main/Source/rcc700.c` is rcc700 (upstream commit `8cc0639`), unmodified.
`main/Source/tactility.c` replaces:
- `main()`, to require an input file and name the default output after it
- `open()`, to translate the newlib flags that rcc700 hardcodes

## License

rcc700 is licensed under the MIT license, see [LICENSE](LICENSE).
