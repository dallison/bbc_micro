# Running on Windows

The machine window uses GDI, and sound uses waveOut. Both are part of every Windows install, so `bbc.exe` needs no extra DLLs. The programs link against the Universal C Runtime, which Windows 10 and 11 include. Windows 7 and 8 need update KB2999226.

The build uses MinGW-w64 GCC or clang. Visual C++ is not supported.

## Build on Windows

Install [MSYS2](https://www.msys2.org/), open the **MSYS2 UCRT64** shell, and install the compiler and CMake:

```
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja
```

From the repository root:

```
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
```

The programs are `build/bbc.exe`, `build/cumana.exe`, `build/cassette.exe`, and the headless `build/6502.exe`. They are linked statically, so they can be copied to another Windows machine without the MSYS2 DLLs.

## Build on Linux and run under Wine

A Linux machine can build the Windows programs and run them under [Wine](https://www.winehq.org/). `windows_tools.sh` fetches a MinGW-w64 cross compiler (llvm-mingw) and a portable Wine into `.wintools`. It needs no root, and deleting `.wintools` removes both.

```
./windows_tools.sh
export PATH="$PWD/.wintools/llvm-mingw/bin:$PWD/.wintools/wine/bin:$PATH"
cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake
cmake --build build-win
ctest --test-dir build-win
```

When Wine is on `PATH` at configure time, `ctest` runs each test `.exe` under it. Start the window the same way:

```
wine build-win/bbc.exe
wine build-win/cumana.exe blank.ssd
```

Wine keeps its settings in `~/.wine`. Set `WINEPREFIX` to keep a separate one, and `WINEDEBUG=-all` to silence its log.

## ROMs

MOS and sideways ROM images are not in this repository. Put your own copies in a folder at the repository root, then run `bbc.exe` from that root so it finds them. `bbc.exe` also looks for the folders beside the program and in the folder above it, so `build\bbc.exe` finds them when started from Explorer.

```
bbc_b_rom_sockets\os.rom
bbc_b_rom_sockets\15-basic2.rom
bbc_b_rom_sockets\14-dfs.rom
```

`os.rom` or `os-<name>.rom` is the operating system. `<socket>-<name>.rom` is sideways socket 0–15. The Master uses `bbc_master_rom_sockets` instead. `-roms dir` names another folder.

```
build\bbc.exe
build\bbc.exe -machine master
build\bbc.exe -volume 7
```

`-volume` is 0 for silence through 11 for full volume. The default is 7. Sound goes to the default output device. Messages such as the ROM list print to the Command Prompt or PowerShell window that started `bbc.exe`.

## Keys

F1 to F9 are the BBC keys f1 to f9, and F10 is f0. F12 is Break. End is COPY. The arrow keys, Escape, Tab, Return, and Backspace map to the same BBC keys. The numeric keypad maps to the Master keypad. Alt+F4 closes the window. The left mouse button is joystick fire 0, and the right button is fire 1.

## Discs and tapes

`cumana.exe` and `cassette.exe` work as described in [README.md](README.md). Both connect to `127.0.0.1`, so they do not need a firewall exception. In a console, `cassette.exe` takes `r` to rewind and `q` to eject.

## Econet

Econet is a separate network from the RS423 serial port. Two processes on the same machine share a wire. Econet listens for UDP on every interface, so Windows Firewall may ask whether to allow `bbc.exe` the first time a station starts. The steps are in [ECONET.md](ECONET.md).
