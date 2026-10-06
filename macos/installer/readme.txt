BBC Micro.app is the machine. The other programs run from a terminal in /Applications/BBC Micro:

  ./cumana blank.ssd
  ./fileserver ~/econet
  ./cassette blank.uef
  ./6502
  ./blank_disc.py blank.ssd
  ./blank_tape.py blank.uef
  ./copy_from_disc.py disc.ssd saved
  ./copy_to_hd.py disc.ssd disc.hd
  ./examine_disc.py disc.adl

The disc scripts need python3.

bbc_b_rom_sockets is the Model B. bbc_master_rom_sockets is the Master. Each folder contains a README.txt that explains how to name a ROM image, including extra images that share one socket. The application looks for those folders beside itself.

This package is built for Apple silicon.
