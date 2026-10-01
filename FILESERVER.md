# File server

`fileserver` is a separate program. It answers as an Econet file server, and the directory you name is the disc `$` the BBC sees.

Build it, then start the server and the emulator on the same wire:

```
cmake --build build --target fileserver bbc
./build/fileserver ~/econet
./build/bbc -econet 1
```

The default station is 254, which is the file server a BBC looks for. The default UDP port is 8179, the same port as `-econet`. On a Mac that wire stays on this machine. Use a `bbc` built with this tree so the emulator joins it.

`-station` chooses another station from 1 to 254. `-port` chooses another UDP port, and `bbc -econet-port` must use that same number:

```
./build/fileserver -port 8180 ~/econet
./build/bbc -econet 1 -econet-port 8180
```

## On the BBC

Put an NFS or ANFS ROM in a sideways socket, named like the other ROM images, for example `12-nfs.rom` in `bbc_b_rom_sockets` or `bbc_master_rom_sockets`. The tree does not include one.

On a Model B, hold N and press F12 to select the network filing system. On a Master, `-econet` stores the station number in CMOS. Then log on:

```
*I AM yourname
```

A subdirectory of that name, when it exists, is your home. Otherwise the whole directory is. `$.LIBRARY` is the library when that directory exists.

`*CAT`, `*DIR`, `*LIB`, `*SAVE`, `*LOAD`, `*DELETE`, `*CDIR`, `*INFO`, `*ACCESS`, and `*BYE` are the commands the server answers.

## Passwords and file details

Create `passwd` in the root, one `NAME SECRET` on each line, to require passwords. Lines starting with `#` are skipped. With no `passwd` file, any `*I AM` succeeds.

Load and execution addresses are kept beside each file in `NAME.inf`.
