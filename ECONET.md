# Econet

Econet is not the RS423 port. RS423 is the 6850 serial chip at `&FE08`. Econet is the 68B54 network chip at `&FEA0`, the station-number links read at `&FE18`, and the same NMI line as the disc.

The interface stays absent until `-econet` names a station from 1 to 254. Station 0 and station 255 are reserved. Two emulators on the same port share one wire. The network clock is on, so a station does not report "No clock".

There is no file server inside the emulator. A second `bbc` is the other station. Put an Econet ROM (NFS or ANFS) in a sideways socket, named like the other ROM images, for example `12-nfs.rom` in `bbc_b_rom_sockets` or `bbc_master_rom_sockets`.

```
./build/bbc -econet 1
./build/bbc -econet 254
```

On the Model B the station number is those links. Hold N and press F12 to select the network filing system. On a Master, `-econet` also stores that station number in CMOS.

The default wire is UDP port 8179. `-econet-port` chooses a different one, which keeps two networks on the same machine apart:

```
./build/bbc -econet 1 -econet-port 8180
./build/bbc -econet 2 -econet-port 8180
```

The headless interpreter uses the same interface with `-bbc-econet` and `-bbc-econet-port`.
