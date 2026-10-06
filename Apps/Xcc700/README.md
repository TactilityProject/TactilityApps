# xcc700

[xcc700](https://github.com/valdanylchuk/xcc700) is a self-hosting mini C compiler for ESP32 (Xtensa).

## Usage

Run it from the Terminal app:

```
xcc700 FILE.c [-o OUTPUT]
```

Without `-o`, the output is named after the input: `something.c` becomes `something.elf`.
The output is a relocatable Xtensa ELF for the [ESP-IDF elf_loader](https://components.espressif.com/components/espressif/elf_loader/).

It supports a small subset of C: see the [xcc700 README](https://github.com/valdanylchuk/xcc700#what-is-missing).

## Source

`main/Source/xcc700.c` is xcc700 (upstream commit `a6179d2`), unmodified.
`main/Source/tactility.c` replaces:
- `main()`, to require an input file and name the default output after it
- `open()`, to translate the Linux flags that xcc700 hardcodes

## License

xcc700 is licensed under the MIT license, see [LICENSE](LICENSE).
