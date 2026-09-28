# bbc_micro

A BBC Micro emulator: Model B and Master 128 / 256. The CPU is an NMOS 6502 on the Model B, a 65C02 when asked, and the Master's G65SC12. It also loads davecc 65C02 ELF programs and serves their `$EF` syscalls on the host.

MOS and sideways ROM images are not in this repository. Put your own copies in the socket directories below.

## Build

Bazel:

```
bazel build //:bbc //:6502
bazel test //:bbc_hardware_test //:6502_nmos_test
```

CMake:

```
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

`bbc` is the Mac window (Cocoa and AudioToolbox). `6502` is the headless interpreter and is the program davecc uses to run a 65C02 executable. On the command line it is still named `6502`.

## ROMs

`bbc_b_rom_sockets` is the Model B. `bbc_master_rom_sockets` is the Master. `os.rom` or `os-<name>.rom` is the operating system. `<socket>-<name>.rom` is sideways socket 0–15, for example `15-basic2.rom` and `14-adfs-1.30.rom`.

```
bazel-bin/bbc
bazel-bin/bbc -machine master -disc blank.adf
```

`-machine` is `b`, `master`, `master128`, or `master256`. `master` and `master128` use a 65SC12, a WD1770, and sideways RAM in sockets 4–7. `master256` makes every socket RAM. `-sram 4`, `8`, `16`, or a list such as `4-7` replaces that set.

The Model B is an NMOS 6502. `-65c02` keeps the CMOS opcodes and the davecc `$EF` syscall on that machine. The Master's `$EF` is a one-byte NOP.

## Discs

```
python3 blank_disc.py blank.ssd
python3 blank_disc.py blank.adf
python3 blank_disc.py blank.adl
```

`.ssd` and `.dsd` are DFS. `.adf` and `.adm` are single-sided ADFS. `.adl` is double-sided ADFS.

## davecc

A davecc 65C02 program runs under `6502`. The interpreter looks for the davecc runtime ROM at `DAVECC_6502_ROM`, under `DAVECC_ROOT`, or beside the `6502` binary as `6502rom.exe`. Pass `-rom file` to name it yourself.

```
6502 -rom /path/to/6502rom.exe program.exe
```

`6502 -bbc` and `bbc -65c02` are the same machine with the BBC memory map, so a davecc program can be the thing that machine runs. The Master CPU does not use `$EF` as a syscall.
