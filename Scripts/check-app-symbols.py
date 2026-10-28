#!/usr/bin/env python3
"""
Check which symbols an ELF app needs that the firmware does not export.

Apps are relocated at load time against the firmware's symbol tables. A symbol the app references
but the firmware does not export fails at *load* time with a bare

    E (9713) ELF: Can't find common xPortEnterCriticalTimeout
    E (9716) ElfApp: Application failed to load: missing symbol

which names one symbol per attempt, so finding them all by flashing is a slow loop. This reads the
app's ELF directly and reports every missing symbol at once, tells you which file to add it to, and
warns about the "funky" cases (mangled C++ names, symbols that exist in the source tree but whose
owning module is never started on the currently-built board).

Usage:
    python Scripts/check-app-symbols.py <path/to/App.app.elf> [--sdk <Tactility path>]
    python Scripts/check-app-symbols.py <path/to/App.app.elf> --out report.txt
    python Scripts/check-app-symbols.py --list-exports [--sdk <Tactility path>]

Exit status is 1 when symbols are missing (or resolve only through an inactive module), so this can
gate a build.
"""

import argparse
import re
import struct
import subprocess
import sys
from pathlib import Path


# ---------------------------------------------------------------------------------------------
# Discovering the firmware checkout
# ---------------------------------------------------------------------------------------------

def find_firmware_root(explicit):
    """Locates the Tactility checkout holding the export tables."""
    if explicit:
        root = Path(explicit)
        if not (root / "TactilityKernel").is_dir():
            raise SystemExit(f"{root} does not look like a Tactility checkout (no TactilityKernel/)")
        return root

    here = Path(__file__).resolve()
    candidates = [
        here.parent.parent.parent / "Tactility",
        here.parent.parent,
    ]
    for candidate in candidates:
        if (candidate / "TactilityKernel" / "source" / "module.cpp").is_file():
            return candidate
    raise SystemExit("Could not locate Tactility next to this repo. Pass --sdk <path> explicitly.")


# Directories under the SDK checkout that are never worth scanning for exports: build output
# (regenerated, huge, and duplicates the real sources) and third-party vendor trees pulled in by
# the component manager (their own internals aren't things an app should ever reference/export).
SKIP_DIR_NAMES = {"build", "managed_components", ".git", "cmake-build-debug"}


def _iter_source_files(root):
    for path in root.rglob("*"):
        if path.suffix not in (".c", ".cpp"):
            continue
        if any(part in SKIP_DIR_NAMES for part in path.parts):
            continue
        yield path


# ---------------------------------------------------------------------------------------------
# Parsing ModuleSymbol export tables out of arbitrary firmware source files
# ---------------------------------------------------------------------------------------------

# DEFINE_MODULE_SYMBOL(name) -> { "name", (void*)&name }
MACRO_RE = re.compile(r"\bDEFINE_MODULE_SYMBOL\(\s*([A-Za-z_][A-Za-z_0-9]*)\s*\)")
# DEFINE_MODULE_SYMBOL_SIGNATURE(name, type) -> exported under bare `name`, address cast to `type`
# to disambiguate an overloaded symbol (e.g. picking one of several float/double overloads).
SIGNATURE_RE = re.compile(r"\bDEFINE_MODULE_SYMBOL_SIGNATURE\(\s*([A-Za-z_][A-Za-z_0-9]*)\s*,")
# DEFINE_MODULE_SYMBOL_ALIAS("name", expr) -> exported under the quoted string, not the C expr.
ALIAS_RE = re.compile(r'\bDEFINE_MODULE_SYMBOL_ALIAS\(\s*"([^"]+)"\s*,')
# Raw { "name", ... } literal table entries: used for forcing-wrapper functions (constructors /
# destructors with no out-of-line definition the compiler would otherwise emit an address for),
# for mangled C++ ABI names GCC can't spell as a C identifier, and for a handful of std:: globals.
LITERAL_RE = re.compile(r'\{\s*"([^"]+)"\s*,')
# Where a Module struct's `.symbols = X` (or positional 4th initializer) names its own symbol
# table, so a table can be tied back to the module (and therefore board) that owns it.
MODULE_STRUCT_RE = re.compile(r"\b(?:struct\s+)?Module\s+([A-Za-z_][A-Za-z_0-9]*)\s*=\s*\{")
SYMBOLS_FIELD_RE = re.compile(r"\.symbols\s*=\s*(?:static_cast<[^>]*>\()?\(?(?:const struct ModuleSymbol\s*\*\s*\)?)?([A-Za-z_][A-Za-z_0-9]*)")
SYMBOL_ARRAY_DECL_RE = re.compile(
    r"\b(?:static\s+)?(?:const\s+)?(?:struct\s+)?ModuleSymbol\s+([A-Za-z_][A-Za-z_0-9]*)\s*\[\s*\]\s*=\s*\{"
)


class ExportEntry:
    __slots__ = ("name", "file", "line", "kind")

    def __init__(self, name, file, line, kind):
        self.name = name
        self.file = file
        self.line = line
        self.kind = kind  # "macro" | "signature" | "alias" | "literal"


def _find_block_end(text, start_index):
    """Given the index of the opening '{' of an initializer, returns the index just past its
    matching '}' (brace-depth aware, so nested { } inside the array don't end it early)."""
    depth = 0
    i = start_index
    n = len(text)
    while i < n:
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return n


def collect_exports(root):
    """Returns (entries, array_owner, module_file, entry_array):
    - entries: every exported ModuleSymbol table entry found anywhere in the firmware tree
    - array_owner: SYMBOLS array C identifier -> the Module identifier whose `.symbols` field
      points at it (when found in the same file)
    - module_file: Module C identifier -> the file it's defined in
    - entry_array: id(entry) -> the SYMBOLS array identifier it came from

    array_owner/module_file/entry_array together let a symbol be traced back to the module that
    must be *started* for it to actually resolve at runtime (see collect_active_modules)."""
    entries = []
    array_owner = {}
    module_file = {}
    entry_array = {}  # id(entry) -> array identifier

    for path in _iter_source_files(root):
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        rel_path = str(path.relative_to(root))

        for m in MODULE_STRUCT_RE.finditer(text):
            module_name = m.group(1)
            brace_index = text.index("{", m.end() - 1)
            block_end = _find_block_end(text, brace_index)
            block = text[brace_index:block_end]
            field_match = SYMBOLS_FIELD_RE.search(block)
            if field_match:
                array_owner[field_match.group(1)] = module_name
            module_file[module_name] = rel_path

        for decl in SYMBOL_ARRAY_DECL_RE.finditer(text):
            array_name = decl.group(1)
            brace_index = text.rindex("{", 0, decl.end())
            block_end = _find_block_end(text, brace_index)
            block_text = text[brace_index:block_end]
            start_line = text.count("\n", 0, brace_index) + 1

            for line_offset, line in enumerate(block_text.splitlines()):
                stripped = line.lstrip()
                if stripped.startswith("//") or stripped.startswith("*") or stripped.startswith("/*"):
                    continue
                line_no = start_line + line_offset

                for name in MACRO_RE.findall(line):
                    e = ExportEntry(name, rel_path, line_no, "macro")
                    entries.append(e)
                    entry_array[id(e)] = array_name
                for name in SIGNATURE_RE.findall(line):
                    e = ExportEntry(name, rel_path, line_no, "signature")
                    entries.append(e)
                    entry_array[id(e)] = array_name
                for name in ALIAS_RE.findall(line):
                    e = ExportEntry(name, rel_path, line_no, "alias")
                    entries.append(e)
                    entry_array[id(e)] = array_name
                if "DEFINE_MODULE_SYMBOL" not in line:
                    for name in LITERAL_RE.findall(line):
                        e = ExportEntry(name, rel_path, line_no, "literal")
                        entries.append(e)
                        entry_array[id(e)] = array_name

    return entries, array_owner, module_file, entry_array


# ---------------------------------------------------------------------------------------------
# Figuring out which modules are actually started on the currently-built board
# ---------------------------------------------------------------------------------------------

# Modules Tactility.cpp starts unconditionally (or near-unconditionally) on every board, in
# addition to kernel_module (always, via kernel_init) and whatever kernel_init's dts_modules[]
# argument names for the specific board that was last built. See Tactility::run() in
# Tactility/Source/Tactility.cpp - kept as a literal list here because it's a handful of fixed
# names, not something worth re-deriving by parsing that function's body every run.
ALWAYS_STARTED_MODULES = {
    "kernel_module",
    "c_symbols_module",
    "cjson_module",
    "posix_symbols_module",  # gated on TT_IS_POSIX/ESP_PLATFORM - true for every real device build
    "cpp_symbols_module",
    "freertos_module",
    "pthread_module",
    "http_module",
    "app_module",
    "crypt_module",
    "mbedtls_module",
    "gps_module",
    "gps_generic_module",
    "gps_meshtastic_module",
    "app_esp32_module",  # ESP_PLATFORM branch; app_posix_module is the POSIX-host alternative
    "app_posix_module",
    "tactility_audio_module",
    "lvgl_window_manager_module",
    "lvgl_module",
}

DTS_MODULES_ARRAY_RE = re.compile(r"\bModule\s*\*\s*const\s+dts_modules\s*\[\s*\]\s*=\s*\{")
IDENTIFIER_RE = re.compile(r"&?([A-Za-z_][A-Za-z_0-9]*)")


def collect_active_modules(sdk_root):
    """Returns (active_module_names, board_name) describing the board that was last built into
    Tactility/build, by reading its generated devicetree.c. Returns (None, None) if the
    firmware hasn't been built yet (no build/ present) - the caller then skips this check
    entirely rather than falsely reporting every module as inactive."""
    devicetree_c = sdk_root / "build" / "esp-idf" / "Tactility" / "Generated" / "devicetree.c"
    if not devicetree_c.is_file():
        return None, None

    text = devicetree_c.read_text(encoding="utf-8", errors="replace")

    board_name = None
    root_config_match = re.search(r"root_config_dt\s+\w+\s*=\s*\{\s*\"([^\"]+)\"", text)
    if root_config_match:
        board_name = root_config_match.group(1)

    dts_match = DTS_MODULES_ARRAY_RE.search(text)
    if not dts_match:
        return None, board_name

    block_end = _find_block_end(text, text.index("{", dts_match.end() - 1))
    block = text[dts_match.end() - 1 : block_end]
    dts_names = {name for name in IDENTIFIER_RE.findall(block) if name != "NULL"}

    return ALWAYS_STARTED_MODULES | dts_names, board_name


# ELF e_machine values for the chip families Tactility targets - EM_XTENSA covers esp32/esp32s3
# (can't tell those two apart from this field alone), EM_RISCV covers esp32c6/esp32p4.
_EM_XTENSA = 0x5E
_EM_RISCV = 0xF3
_ELF_MACHINE_FAMILY = {_EM_XTENSA: "xtensa", _EM_RISCV: "riscv"}
_TARGET_FAMILY = {
    "esp32": "xtensa", "esp32s3": "xtensa",
    "esp32c6": "riscv", "esp32p4": "riscv",
}


def get_built_target(sdk_root):
    """Returns the IDF_TARGET (e.g. 'esp32s3') Tactility/build was last configured for, or
    None if it hasn't been built. The active-module cross-check above is only meaningful when the
    app ELF was built for the SAME chip as this firmware build - a module active on an esp32s3
    board tells you nothing about what's active on an esp32p4 one."""
    sdkconfig_json = sdk_root / "build" / "config" / "sdkconfig.json"
    if not sdkconfig_json.is_file():
        return None
    try:
        import json
        return json.loads(sdkconfig_json.read_text(encoding="utf-8")).get("IDF_TARGET")
    except (OSError, ValueError):
        return None


def guess_elf_target_family(elf_path):
    """Returns 'xtensa' or 'riscv' from the ELF header's e_machine field, or None if unrecognized."""
    with open(elf_path, "rb") as f:
        header = f.read(20)
    if len(header) < 20 or header[:4] != b"\x7fELF":
        return None
    e_machine = struct.unpack_from("<H", header, 0x12)[0]
    return _ELF_MACHINE_FAMILY.get(e_machine)


# ---------------------------------------------------------------------------------------------
# Reading the app ELF's undefined symbol references
# ---------------------------------------------------------------------------------------------

def read_undefined_symbols(elf_path):
    """Returns the set of symbol names a 32-bit little-endian ELF references externally."""
    data = elf_path.read_bytes()

    if data[:4] != b"\x7fELF":
        raise SystemExit(f"{elf_path} is not an ELF file")
    if data[4] != 1:
        raise SystemExit(f"{elf_path} is not 32-bit; this script handles ELF32 only")

    section_header_offset = struct.unpack_from("<I", data, 0x20)[0]
    section_header_size = struct.unpack_from("<H", data, 0x2E)[0]
    section_count = struct.unpack_from("<H", data, 0x30)[0]
    section_name_index = struct.unpack_from("<H", data, 0x32)[0]

    sections = []
    for index in range(section_count):
        offset = section_header_offset + index * section_header_size
        name, sh_type, _flags, _addr, off, size, link, _info, _align, _entsize = struct.unpack_from(
            "<10I", data, offset
        )
        sections.append({"name": name, "type": sh_type, "off": off, "size": size, "link": link})

    name_table_offset = sections[section_name_index]["off"]

    def section_name(offset):
        end = data.index(b"\0", name_table_offset + offset)
        return data[name_table_offset + offset : end].decode()

    for section in sections:
        section["label"] = section_name(section["name"])

    dynsym = next((s for s in sections if s["label"] == ".dynsym"), None)

    def symbol_name(symtab, sym_index):
        string_table = sections[symtab["link"]]
        offset = symtab["off"] + sym_index * 16
        name_offset = struct.unpack_from("<I", data, offset)[0]
        if name_offset == 0:
            return None
        end = data.index(b"\0", string_table["off"] + name_offset)
        return data[string_table["off"] + name_offset : end].decode()

    def symbol_shndx(symtab, sym_index):
        # Elf32_Sym: name(4) value(4) size(4) info(1) other(1) shndx(2) = 16 bytes; shndx at +14.
        offset = symtab["off"] + sym_index * 16
        return struct.unpack_from("<H", data, offset + 14)[0]

    referenced = set()

    # Primary path: walk every SHT_RELA section's relocations and collect the symbol each one
    # references, the same symbols esp_elf.c's relocate loop processes. A relocation naming a
    # symbol that's only DECLARED in this ELF (st_shndx == SHN_UNDEF, i.e. never defined here)
    # genuinely needs the firmware to resolve it. One that's already DEFINED here - typically a
    # weak/COMDAT template instantiation the app's own compiler emitted a local copy of, e.g. a
    # shared_ptr<AppType> control-block constructor - does not: esp_elf.c's relocate loop falls
    # back to the symbol's own local address whenever the firmware doesn't provide one (confirmed
    # against esp_elf.c's `if (!addr && sym->shndx != SHN_UNDEF)` fallback, and against a real
    # device: EpubReader's shared_ptr<EpubService> constructor loads and runs fine with no
    # firmware-side export at all, because its own ELF already carries a real definition of it).
    #
    # Each RELA section's own sh_link names the symbol table its entries index into - not
    # necessarily .dynsym, though it is for every app ELF side-loading currently produces. Resolved
    # per-section rather than hardcoded so a differently-built ELF (e.g. a future toolchain/link
    # config with a separate .symtab) doesn't silently misattribute relocations to the wrong table.
    SHT_RELA = 4
    found_rela = False
    for section in sections:
        if section.get("type") != SHT_RELA:
            continue
        symtab = sections[section["link"]] if section["link"] < len(sections) else dynsym
        if symtab is None or symtab["label"] not in (".dynsym", ".symtab"):
            continue
        found_rela = True
        symtab_entry_count = symtab["size"] // 16
        entry_count = section["size"] // 12  # Elf32_Rela: r_offset, r_info, r_addend
        for index in range(entry_count):
            offset = section["off"] + index * 12
            _r_offset, r_info, _r_addend = struct.unpack_from("<III", data, offset)
            sym_index = r_info >> 8
            if sym_index == 0 or sym_index >= symtab_entry_count:
                continue
            if symbol_shndx(symtab, sym_index) != 0:  # SHN_UNDEF == 0: already defined locally
                continue
            name = symbol_name(symtab, sym_index)
            if name:
                referenced.add(name)

    if found_rela:
        return referenced

    # Fallback for a plain (non-PIC) ELF with no relocations to walk: treat SHN_UNDEF .dynsym/
    # .symtab entries as undefined, the simpler model that held before shared-object-style apps.
    undefined = set()
    for section in sections:
        if section["label"] not in (".dynsym", ".symtab"):
            continue
        string_table = sections[section["link"]]
        entry_count = section["size"] // 16
        for index in range(entry_count):
            offset = section["off"] + index * 16
            name_offset, _value, _size, _info, _other, shndx = struct.unpack_from(
                "<IIIBBH", data, offset
            )
            if shndx != 0 or name_offset == 0:
                continue
            end = data.index(b"\0", string_table["off"] + name_offset)
            undefined.add(data[string_table["off"] + name_offset : end].decode())
    return undefined


# ---------------------------------------------------------------------------------------------
# Making missing symbols legible: demangling + "where does this belong" heuristics
# ---------------------------------------------------------------------------------------------

_CXXFILT_CACHE = {}
_cxxfilt_checked = False
_cxxfilt_path = None


def _find_cxxfilt():
    global _cxxfilt_checked, _cxxfilt_path
    if _cxxfilt_checked:
        return _cxxfilt_path
    _cxxfilt_checked = True
    import shutil

    for candidate in ("xtensa-esp32-elf-c++filt", "c++filt"):
        found = shutil.which(candidate)
        if found:
            _cxxfilt_path = found
            break
    return _cxxfilt_path


def demangle(name):
    """Best-effort Itanium C++ demangling via c++filt, if one is on PATH. Returns None if the
    name isn't mangled or no demangler is available (never raises - this is a diagnostic aid,
    not a correctness requirement)."""
    if not name.startswith("_Z"):
        return None
    if name in _CXXFILT_CACHE:
        return _CXXFILT_CACHE[name]

    cxxfilt = _find_cxxfilt()
    if not cxxfilt:
        _CXXFILT_CACHE[name] = None
        return None

    try:
        result = subprocess.run(
            [cxxfilt, name], capture_output=True, text=True, timeout=5, check=False
        )
        demangled = result.stdout.strip() or None
        if demangled == name:
            demangled = None
    except (OSError, subprocess.SubprocessError):
        demangled = None

    _CXXFILT_CACHE[name] = demangled
    return demangled


# Heuristics for "which export file should this symbol be added to", ordered most-specific-first.
# Each entry is (predicate, human_label, suggested_file). Checked against both the raw and (when
# it demangles) the demangled name.
_LIBC_MATH_RE = re.compile(r"^(sin|cos|tan|asin|acos|atan|atan2|sinh|cosh|tanh|asinh|acosh|atanh|"
                            r"exp|exp2|expm1|log|log2|log10|log1p|pow|sqrt|cbrt|hypot|fabs|floor|"
                            r"ceil|round|trunc|fmod|remainder|copysign|nextafter|fdim|fmax|fmin|"
                            r"ldexp|frexp|modf|isnan|isinf)[fl]?$")

_CATEGORY_RULES = [
    (lambda n, d: n.startswith("_ZSt") or n.startswith("_ZN9__gnu_cxx") or "std::" in (d or ""),
     "C++ standard library (mangled)",
     "Modules/cpp-symbols-module/source/module.cpp"),
    (lambda n, d: n in ("_Znwj", "_Znaj", "_ZdlPvj", "_ZdaPvj", "_ZdlPv", "_ZdaPv") or "operator new" in (d or "") or "operator delete" in (d or ""),
     "C++ new/delete operator",
     "Modules/cpp-symbols-module/source/module.cpp"),
    (lambda n, d: n.startswith("__cxa_") or n.startswith("_ZTI") or n.startswith("_ZTV"),
     "C++ ABI runtime support (RTTI/guards/exceptions)",
     "Modules/cpp-symbols-module/source/module.cpp"),
    (lambda n, d: n.startswith("pthread_"),
     "POSIX threads",
     "Modules/pthread-module/source/module.cpp"),
    (lambda n, d: n.startswith(("vfs_", "open", "close", "read", "write", "lseek", "fstat", "stat", "unlink", "mkdir", "opendir", "readdir")) and not n.startswith("_Z"),
     "POSIX file/VFS API",
     "Modules/posix-symbols-module/source/module.cpp"),
    (lambda n, d: n.startswith(("xPort", "vPort", "xTask", "vTask", "uxTask", "xQueue", "vQueue", "xSemaphore", "xEventGroup", "xTimer", "vTimer")),
     "FreeRTOS kernel primitive",
     "Modules/freertos-module/source/module.cpp"),
    (lambda n, d: bool(_LIBC_MATH_RE.match(n)),
     "libc/libm math function",
     "Modules/c-symbols-module/source/module.cpp"),
    (lambda n, d: n.startswith(("malloc", "calloc", "realloc", "free", "strtod", "strtol", "strtoul", "atoi", "atof", "atol", "qsort", "rand", "srand", "getenv")) and not n.startswith("_Z"),
     "libc stdlib function",
     "Modules/c-symbols-module/source/module.cpp"),
    (lambda n, d: n.startswith(("str", "mem")) and not n.startswith("_Z") and n.islower(),
     "libc string/mem function",
     "Modules/c-symbols-module/source/module.cpp"),
    (lambda n, d: n.startswith("lv_"),
     "LVGL API",
     "Modules/lvgl-module/source/symbols.c"),
    (lambda n, d: n.startswith(("esp_", "gpio_", "i2c_", "spi_", "adc_", "uart_")),
     "ESP-IDF driver/HAL API",
     "Platforms/platform-esp32/source/module.cpp"),
    (lambda n, d: n.startswith("tt_") or n.startswith("tt::"),
     "Tactility app-facing API",
     "TactilityC/Source/tt_*.cpp (or the relevant TactilityKernel/source/symbols.c entry)",
     ),
]


def classify_symbol(name):
    """Returns (label, suggested_file, is_mangled, demangled_or_none)."""
    demangled = demangle(name)
    is_mangled = name.startswith("_Z")
    for rule in _CATEGORY_RULES:
        predicate = rule[0]
        label = rule[1]
        suggestion = rule[2]
        if predicate(name, demangled):
            return label, suggestion, is_mangled, demangled
    return "Unrecognized / app-specific", None, is_mangled, demangled


# ---------------------------------------------------------------------------------------------
# Report rendering
# ---------------------------------------------------------------------------------------------

def render_report(elf_path, exports_by_name, entry_array, array_owner, module_file,
                   active_modules, board_name, undefined, resolved_verbose):
    lines = []
    w = lines.append

    missing = sorted(undefined - exports_by_name.keys())
    resolved_names = sorted(undefined & exports_by_name.keys())

    # Split "resolved" into genuinely-active vs. resolves-in-source-but-module-never-started.
    # A name is inactive only when EVERY entry has a known owner and ALL owners are inactive;
    # a single active (or unknown-owner) entry keeps it working at runtime.
    resolved_active = []
    resolved_inactive = []
    for name in resolved_names:
        owners = set()
        unknown_owner = False
        for entry in exports_by_name[name]:
            array_name = entry_array.get(id(entry))
            owning_module = array_owner.get(array_name) if array_name else None
            if owning_module is None:
                unknown_owner = True
            else:
                owners.add(owning_module)
        if active_modules is None or unknown_owner or any(m in active_modules for m in owners):
            resolved_active.append(name)
        else:
            resolved_inactive.append((name, sorted(owners)[0] if owners else None))

    w(f"App ELF          : {elf_path}")
    w(f"Firmware exports : {len(exports_by_name)} symbols across the source tree")
    if board_name:
        w(f"Built board      : {board_name}")
    if active_modules is not None:
        w(f"Active modules   : {len(active_modules)} (from Tactility/build's devicetree.c)")
    else:
        w("Active modules   : unknown (Tactility hasn't been built yet - skipping the "
          "inactive-module check; build the firmware once for a fully accurate report)")
    w(f"App references   : {len(undefined)} externally-resolved symbols")
    w(f"Resolved         : {len(resolved_active)}")
    if resolved_inactive:
        w(f"Resolved*        : {len(resolved_inactive)} (*only via a module not started on this board - see below)")
    w(f"Missing          : {len(missing)}")

    if resolved_verbose and resolved_active:
        w("\nResolved symbols:")
        for name in resolved_active:
            for entry in exports_by_name[name]:
                w(f"  {name}  ({entry.file}:{entry.line})")

    if resolved_inactive:
        w("\n" + "=" * 78)
        w("SYMBOLS THAT RESOLVE IN SOURCE BUT WON'T LOAD ON THIS BOARD")
        w("=" * 78)
        w("These are declared in a ModuleSymbol table somewhere in Tactility, but that")
        w("module is never started for the board currently built into Tactility/build.")
        w("They will fail to load with the exact same \"Can't find <symbol>\" error as a")
        w("truly-missing symbol. Either the app shouldn't need them on this board, or the")
        w("module needs to be added to this board's device tree / Tactility::run().")
        for name, owning_module in resolved_inactive:
            owner_file = module_file.get(owning_module, "?")
            w(f"\n  {name}")
            for entry in exports_by_name[name]:
                w(f"    exported by  : {entry.file}:{entry.line}")
            w(f"    owning module: {owning_module}  ({owner_file})")

    if missing:
        w("\n" + "=" * 78)
        w("MISSING SYMBOLS - the app will fail to load")
        w("=" * 78)
        for name in missing:
            label, suggested_file, is_mangled, demangled = classify_symbol(name)
            w(f"\n  {name}")
            if demangled:
                w(f"    demangled    : {demangled}")
            if is_mangled and not demangled:
                w("    demangled    : (c++filt not found on PATH - install binutils to decode this mangled name)")
            w(f"    looks like   : {label}")
            if suggested_file:
                w(f"    add it to    : {suggested_file}")
                if is_mangled:
                    w("    how          : mangled C++ name - the identifier isn't valid C, so it can't use")
                    w("                   DEFINE_MODULE_SYMBOL(name) directly. Use a raw literal entry instead:")
                    w(f'                   {{ "{name}", (void*)&<the real C++ name/overload> }}')
                    w("                   (see cpp-symbols-module/source/module.cpp for many examples, including")
                    w("                   forcing-wrapper functions for ctors/dtors with no addressable definition)")
                else:
                    w(f"    how          : DEFINE_MODULE_SYMBOL({name})")
            else:
                w("    add it to    : (no confident match - this looks app/library-specific;")
                w("                   grep the app's own dependencies for where it's declared,")
                w("                   then add a DEFINE_MODULE_SYMBOL/literal entry to the most")
                w("                   relevant existing *-symbols-module or Drivers/*/source/symbols.c)")

        w("\n" + "-" * 78)
        w("After adding the export(s):")
        w("  1. Rebuild the firmware (Tactility) so the new export table is compiled in.")
        w("  2. Reflash, or just re-run the app if you're side-loading the ELF.")
        w("  3. Re-run this script against the freshly-built app ELF to confirm.")
    else:
        w("\nAll referenced symbols resolve against an active module. 🎉" if not resolved_inactive else
          "\nNo missing symbols, but see the inactive-module warnings above.")

    return "\n".join(lines)


# ---------------------------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Report symbols an ELF app needs that the firmware does not export, "
                     "where to add them, and whether they'd even be active on the built board."
    )
    parser.add_argument("elf", nargs="?", help="path to the built <App>.app.elf")
    parser.add_argument("--sdk", help="path to the Tactility checkout")
    parser.add_argument("--out", help="also write the full report to this text file")
    parser.add_argument(
        "--list-exports",
        action="store_true",
        help="print every exported symbol (with source location) and exit",
    )
    parser.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        help="also list the symbols that do resolve",
    )
    args = parser.parse_args()

    root = find_firmware_root(args.sdk)
    entries, array_owner, module_file, entry_array = collect_exports(root)

    # Keep EVERY entry per name: a symbol can be exported by several modules
    # (active and inactive). Collapsing to one entry misclassifies it either
    # way depending on file order, so callers apply any/all owner logic.
    exports_by_name = {}
    for entry in entries:
        exports_by_name.setdefault(entry.name, []).append(entry)

    if args.list_exports:
        for name in sorted(exports_by_name):
            for entry in exports_by_name[name]:
                print(f"{name}  ({entry.kind}, {entry.file}:{entry.line})")
        return 0

    if not args.elf:
        parser.error("an ELF path is required unless --list-exports is given")

    elf_path = Path(args.elf)
    if not elf_path.is_file():
        raise SystemExit(f"No such file: {elf_path}")

    active_modules, board_name = collect_active_modules(root)

    # The active-module list reflects whatever chip Tactility/build was last configured for.
    # If the app ELF was built for a different chip, that list says nothing useful about it - e.g.
    # an esp32p4 app checked while the firmware's last build was esp32s3. Drop the check rather
    # than silently mis-reporting inactive-but-actually-fine (or vice versa) modules.
    built_target = get_built_target(root)
    built_family = _TARGET_FAMILY.get(built_target) if built_target else None
    elf_family = guess_elf_target_family(elf_path)
    target_mismatch = bool(built_family and elf_family and built_family != elf_family)
    if target_mismatch:
        active_modules = None

    undefined = read_undefined_symbols(elf_path)

    report = render_report(
        elf_path, exports_by_name, entry_array, array_owner, module_file,
        active_modules, board_name, undefined, args.verbose,
    )
    if target_mismatch:
        report = (
            f"NOTE: firmware was last built for {built_target} ({built_family}), but this ELF "
            f"looks like a {elf_family} build - skipping the active-module check since it would "
            f"be checked against the wrong board. Build Tactility for this app's target chip "
            f"first if you want that check.\n\n" + report
        )
    print(report)

    if args.out:
        Path(args.out).write_text(report + "\n", encoding="utf-8")
        print(f"\n(report written to {args.out})")

    missing = sorted(undefined - exports_by_name.keys())
    has_inactive = False
    if active_modules is not None:
        for name in undefined & exports_by_name.keys():
            owners = set()
            unknown_owner = False
            for entry in exports_by_name[name]:
                array_name = entry_array.get(id(entry))
                owning_module = array_owner.get(array_name) if array_name else None
                if owning_module is None:
                    unknown_owner = True
                else:
                    owners.add(owning_module)
            if not unknown_owner and owners and all(m not in active_modules for m in owners):
                has_inactive = True
                break

    return 1 if (missing or has_inactive) else 0


if __name__ == "__main__":
    sys.exit(main())
