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

`bbc` is the machine window. On a Mac it uses Cocoa and AudioToolbox. On Linux it uses X11, which a desktop Ubuntu or Debian install already has, and ALSA when those headers were present at build time. Compiling the Linux window needs the X11 headers:

```
sudo apt install build-essential cmake libx11-dev libasound2-dev
```

`libasound2-dev` is only for sound. Step-by-step Ubuntu and Debian instructions are in [LINUX.md](LINUX.md).

On Windows `bbc.exe` uses GDI and waveOut, which every install has. It builds with MinGW-w64 under MSYS2, or on Linux with a cross compiler, and runs there under Wine. Both are in [WINDOWS.md](WINDOWS.md).

`6502` is the headless interpreter and is the program davecc uses to run a 65C02 executable. On the command line it is still named `6502`.

## ROMs

`bbc_b_rom_sockets` is the Model B. `bbc_master_rom_sockets` is the Master. `os.rom` or `os-<name>.rom` is the operating system. `<socket>-<name>.rom` is sideways socket 0–15, for example `15-basic2.rom` and `14-adfs-1.30.rom`.

DFS and ADFS images can both stay in that directory. Each is fitted in the socket named by its file, and Break selects the one in the higher socket, as on the machine. `*DISC` and `*ADFS` select the other. The disc controller follows the ROM that uses the drive, so an 8271 DFS and ADFS can both be present. If both files name the same socket, the second is fitted in the highest free socket. `-fs dfs` or `-fs adfs` loads only that one. `-fdc` forces the controller.

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

`bbc` listens on `127.0.0.1:8177`. `cumana` connects and inserts an image into a drive of the running machine. Closing `cumana` ejects it, and writes the guest makes are stored back into the file.

```
cumana blank.ssd
cumana -drive 1 blank.adf
cumana -drive 2 side.ssd
cumana -drive 0 blank.adl
```

DFS has drives 0–3. Drives 2 and 3 are the second side of drives 0 and 1. ADFS has drives 0 and 1; a double-sided image is one of those drives. With no `-drive`, the emulator uses the first free drive. `-disc-port 0` closes the socket. `-disc` still mounts an image before the machine starts.

## Tapes

`blank_tape.py` writes a UEF cassette image. A filename alone is a blank tape the emulator can record onto. `--data` stores a binary as Acorn cassette blocks, which `*LOAD` and `*RUN` can read.

```
python3 blank_tape.py blank.uef
python3 blank_tape.py --name HELLO --load 0xE00 --exec 0xE00 --data prog.bin hello.uef
```

`bbc` listens on `127.0.0.1:8178`. `cassette` connects and inserts that file. Closing it ejects the tape. A guest `*SAVE` is written back into the file when the cassette motor stops. On a terminal, `r` rewinds and `q` ejects. A missing file starts blank and is created on the first recording. `-ro` is play-only.

```
cassette blank.uef
cassette -ro hello.uef
```

`-tape-port 0` closes the socket. `-tape` still loads a tape before the machine starts, and a guest `*SAVE` is written back into that file when the cassette motor stops. Inside the emulator, `*TAPE` then `*SAVE` records onto whichever tape is inserted.

## Econet

Econet is the 68B54 at `&FEA0`, not the RS423 serial port. How to run two stations is in [ECONET.md](ECONET.md).

## davecc

A davecc 65C02 program runs under `6502`. The interpreter looks for the davecc runtime ROM at `DAVECC_6502_ROM`, under `DAVECC_ROOT`, or beside the `6502` binary as `6502rom.exe`. Pass `-rom file` to name it yourself.

```
6502 -rom /path/to/6502rom.exe program.exe
```

`6502 -bbc` and `bbc -65c02` are the same machine with the BBC memory map, so a davecc program can be the thing that machine runs. The Master CPU does not use `$EF` as a syscall.
