# Running on Linux

The machine window uses X11. A desktop Ubuntu or Debian install already has the library. The compiler needs the headers. Sound uses ALSA when those headers were present at build time. Without them the window still runs, and it stays silent.

## Packages

```
sudo apt install build-essential cmake libx11-dev libasound2-dev
```

`libasound2-dev` is only for sound. Leave it out if you want a silent build.

## Build

From the repository root:

```
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

The window program is `build/bbc`. If CMake prints `libX11 not found`, the `libx11-dev` package is missing and `bbc` was not built. The headless `build/6502` interpreter does not need X11.

Start `bbc` from a desktop session. Over SSH it needs a display (`ssh -X`, or a local session). With no display it prints `bbc: no X display` and exits.

## ROMs

MOS and sideways ROM images are not in this repository. Put your own copies in a directory at the repository root, then run `bbc` from that root so it finds them:

```
bbc_b_rom_sockets/os.rom
bbc_b_rom_sockets/15-basic2.rom
bbc_b_rom_sockets/14-dfs.rom
```

`os.rom` or `os-<name>.rom` is the operating system. `<socket>-<name>.rom` is sideways socket 0–15. The Master uses `bbc_master_rom_sockets` instead. `-roms dir` names another directory.

```
./build/bbc
./build/bbc -machine master
./build/bbc -volume 7
```

`-volume` is 0 for silence through 11 for full volume. The default is 7. Sound goes to the ALSA device named `default`.

## Keys

F1 to F9 are the BBC keys f1 to f9, and F10 is f0. F12 is Break. End is COPY. The arrow keys, Escape, Tab, Return, and Backspace map to the same BBC keys. The numeric keypad maps to the Master keypad. The left mouse button is joystick fire 0, and the right button is fire 1.

## Econet

Econet is a separate network from the RS423 serial port. Two processes on the same machine share a wire. The steps are in [ECONET.md](ECONET.md).

Discs, tapes, and the other flags are in [README.md](README.md).
