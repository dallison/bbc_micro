//
//  bbc_hardware.h
//  BBC Micro Model B hardware attached to the 6502 interpreter.
//
//  FRED  (&FC00-&FCFF)  1 MHz bus page. Unwritten locations read as &FF.
//  JIM   (&FD00-&FDFF)  1 MHz bus window into a 64 KiB RAM, paged by &FCFF.
//  SHEILA (&FE00-&FEFF) CRTC, 6850 ACIA (the RS423 serial port), serial ULA,
//                       video ULA, ROMSEL, the two 6522 VIAs, an 8271 or
//                       WD1770 floppy controller, and the µPD7002 ADC. The
//                       Tube reads as an empty socket. Econet is the 68B54
//                       ADLC at &FEA0-&FEBF and the station links at
//                       &FE18-&FE1F; both read as an empty socket until
//                       BbcMachineOpenEconet fits the interface.
//
//  The video ULA serialises screen RAM (read through the CRTC address
//  translation) into a 24-bit framebuffer. MODE 7 uses a teletext cell
//  renderer. Pass -bbc to the interpreter; without it the hosted ACIA at
//  &FE00 is unchanged.
//

#ifndef bbc_hardware_h
#define bbc_hardware_h

#include "6502_devices.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BBC_FB_WIDTH 640
#define BBC_FB_HEIGHT 512

typedef struct BbcMachine BbcMachine;

// Owns the hardware state, the framebuffer, JIM RAM, and sideways ROM slots.
// Guest RAM is not owned; call BbcMachineSetRam with the interpreter's 64 KiB.
BbcMachine* BbcMachineCreate(void);
void BbcMachineDestroy(BbcMachine* bbc);

void BbcMachineSetRam(BbcMachine* bbc, uint8_t* ram);
void BbcMachineSelectMode(BbcMachine* bbc, int mode);

// 16 KiB OS image. Bytes that would land in &FC00-&FEFF are skipped.
bool BbcMachineLoadOs(BbcMachine* bbc, const char* path);

// Directory of ROM images. The Model B looks in bbc_b_rom_sockets and a
// Master looks in bbc_master_rom_sockets. -roms names a directory instead.
//   os.rom or os-<name>.rom           MOS, for example os-1.20.rom
//   <socket>.rom or <socket>-<name>.rom
//                                     sideways socket 0-15, for example
//                                     15-basic2.rom
#define BBC_B_ROM_DIRECTORY "bbc_b_rom_sockets"
#define BBC_MASTER_ROM_DIRECTORY "bbc_master_rom_sockets"
#define BBC_ROM_IMAGE_MAX 17

// BBC_MACHINE_B is the Model B. BBC_MACHINE_MASTER is the Master 128:
// 65C02, WD1770, ACCCON, and sideways RAM in sockets 4-7. MASTER256 keeps
// that map and makes all sixteen sockets sideways RAM.
#define BBC_MACHINE_B 0
#define BBC_MACHINE_MASTER 1
#define BBC_MACHINE_MASTER256 2

bool BbcMachineSetModel(BbcMachine* bbc, int model);
// ram[n] is true when socket n is sideways RAM. A loaded image is the
// initial contents and stays writable. A Master 128 passes sockets 4-7.
bool BbcMachineSetSidewaysRam(BbcMachine* bbc, const bool ram[16]);
bool BbcMachineIsMaster(const BbcMachine* bbc);
// "b", "master", "master128", "master256". Returns -1 when unknown.
int BbcMachineParseModel(const char* text);
// "4" is sockets 4-7, "8" is 0-7, "16" is every socket. A list such as
// "4-7" or "4,5,6,7" names sockets directly.
bool BbcMachineParseSidewaysRam(const char* text, bool ram[16]);
const char* BbcMachineRomDirectoryName(int model);

typedef struct BbcRomFile {
  int slot;   // -1 is the OS image. 0-15 is a sideways socket.
  char* path;
} BbcRomFile;

// malloc'd path of ./<directory>, or that directory beside the executable
// or its parent. NULL when none of those directories exist.
char* BbcMachineFindRomDirectory(const char* argv0, const char* directory);
// Which filing-system ROM to take from the socket directory. BBC_FS_ANY
// loads every image. DFS and ADFS may share a socket; pass DFS or ADFS to
// load that one and leave the other on disk.
#define BBC_FS_ANY 0
#define BBC_FS_DFS 1
#define BBC_FS_ADFS 2
// "dfs", "disc", "disk", or "adfs". Returns -1 when unknown.
int BbcMachineParseFilingSystem(const char* text);
// BBC_FS_DFS, BBC_FS_ADFS, or BBC_FS_ANY from a ROM file name.
int BbcMachineRomFilingKind(const char* name);
// Fills files with malloc'd paths. BBC_FS_ANY fits both a DFS and an ADFS
// image; a shared socket number puts the second in the highest free socket.
// DFS or ADFS loads only that filing system. Returns the count, or -1.
int BbcMachineListRomDirectory(const char* dir, BbcRomFile* files, int capacity, int filing);
// Controller for a filing-system choice: ADFS and a 1770 DFS use the WD1770,
// and an 8271 DFS uses the 8271. The Master always uses the WD1770.
int BbcMachineControllerForFiling(const BbcMachine* bbc, int filing);
void BbcRomFileFree(BbcRomFile* files, int count);
const char* BbcRomOsPath(const BbcRomFile* files, int count);
// Loads sideways images. skip_slot[n] leaves socket n alone. The OS image
// is not loaded here.
bool BbcMachineLoadRomFiles(BbcMachine* bbc, const BbcRomFile* files, int count,
                            const bool skip_slot[16]);
// Sideways ROM/RAM image for slots 0-15. Shorter files are padded with &FF.
bool BbcMachineLoadSideways(BbcMachine* bbc, int slot, const char* path);
// Floppy images: .ssd/.dsd for DFS, .adf/.adm/.adl for ADFS. Drive is 0 or 1.
// Writes are stored back into the file. DFS and ADFS ROMs are both fitted.
// The controller follows the filing-system ROM that uses the drive: ADFS and
// a 1770 DFS use the WD1770, and an 8271 DFS uses the 8271.
#define BBC_FDC_8271 1
#define BBC_FDC_1770 2
bool BbcMachineLoadDisc(BbcMachine* bbc, int drive, const char* path);
// Listen on 127.0.0.1 for cumana. port 0 lets the kernel choose. Returns the
// bound port, or -1 when the socket cannot be opened. DFS clients use drives
// 0-3; ADFS clients use 0 and 1. Closing the connection ejects the disc.
int BbcMachineListenDiscs(BbcMachine* bbc, int port);
// Listen on 127.0.0.1 for cassette. port 0 lets the kernel choose. Returns
// the bound port, or -1 when the socket cannot be opened. One tape is
// inserted at a time. Closing the connection ejects it.
int BbcMachineListenTapes(BbcMachine* bbc, int port);
// Fit the Econet interface as station 1-254 and join the virtual wire on
// this UDP port. Every emulator using the same port shares one network.
// The default port is BBC_ECONET_PORT. Returns that port, or -1 when the
// station number is invalid or the socket cannot be opened. A socket
// failure still fits the interface, so stations in this process can talk.
#define BBC_ECONET_PORT 8179
int BbcMachineOpenEconet(BbcMachine* bbc, int station, int port);
void BbcMachineSetFdc(BbcMachine* bbc, int kind);
void BbcMachineResetFdc(BbcMachine* bbc);
// Latched by a falling edge on the FDC interrupt. The CPU consumes it.
bool BbcMachineNmiPending(const BbcMachine* bbc);
void BbcMachineClearNmi(BbcMachine* bbc);
bool BbcMachineLoadSidewaysBytes(BbcMachine* bbc, int slot, const uint8_t* bytes,
                                 size_t length);
// Copy the ROMSEL slot into guest RAM at &8000 when any slot has been loaded.
void BbcMachineMapSideways(BbcMachine* bbc);

void BbcMachineWrite(BbcMachine* bbc, uint16_t addr, uint8_t value);
uint8_t BbcMachineRead(BbcMachine* bbc, uint16_t addr);
// The Master E bit maps LYNNE only while the opcode is fetched from
// &C000-&DFFF. Call this with the program counter before the fetch.
void BbcMachineBeginInstruction(BbcMachine* bbc, uint16_t pc);
// A sideways ROM ignores writes. Sideways RAM and the Master's ANDY do not.
bool BbcMachineSidewaysRom(const BbcMachine* bbc, uint16_t addr);
// The MOS image ignores writes. On a Master, &C000-&DFFF is HAZEL RAM while
// ACCCON bit 3 is set; &E000-&FBFF and &FF00-&FFFF stay ROM. DFS 2.45's
// read-track routine stores every byte at &FF00, and a write that sticks
// there replaces the NMI vector.
bool BbcMachineMosRom(const BbcMachine* bbc, uint16_t addr);
// BREAK clears ROMSEL and, on a Master, ACCCON.
void BbcMachineBreak(BbcMachine* bbc);
// Remember &C000-&DFFF as the MOS image HAZEL will page out.
void BbcMachineCaptureMos(BbcMachine* bbc);

// A tone or noise channel is audible. Volume 15 is silent.
bool BbcMachineSounding(const BbcMachine* bbc);
// Cycles until the disc controller must be serviced. Large when it is idle,
// so a memory fill can still update the hardware in blocks.
int BbcMachineDiscHorizon(const BbcMachine* bbc);
// Skip per-cycle video and audio while the CPU runs ahead of the wall clock.
// Turning it off restarts the beam so the next frame is painted normally.
void BbcMachineSetFast(BbcMachine* bbc, bool fast);
// Count 2 MHz CPU cycles. The 6845 beam runs with them, so a mode change
// partway down the frame stays on the scanlines after the change. Vsync
// comes from the CRTC and sets the system VIA CA1 flag.
void BbcMachineAdvance(BbcMachine* bbc, int cpu_cycles);
// Frames the beam has completed. Zero until the CRTC has reached vsync.
int BbcMachineCompletedFrames(const BbcMachine* bbc);
bool BbcMachineIrqPending(const BbcMachine* bbc);

// CRTC memory-address to guest RAM, including the Model B hardware scroll.
uint16_t BbcVideoAddress(const BbcMachine* bbc, uint16_t ma, uint8_t ra);

// Column 0-15, row 0-7. down is the physical key state.
void BbcMachineSetKey(BbcMachine* bbc, int column, int row, bool down);
// Channel 0-3, value 0-65535. Fire buttons are active when down.
void BbcMachineSetAnalogue(BbcMachine* bbc, int channel, int value);
void BbcMachineSetFire(BbcMachine* bbc, int button, bool down);
bool BbcMachineCapsLed(const BbcMachine* bbc);
bool BbcMachineShiftLed(const BbcMachine* bbc);
bool BbcMachineMotorOn(const BbcMachine* bbc);
// UEF or a raw cassette byte stream. Playback starts at the beginning.
// A UEF chunk 0x0110 is carrier and 0x0100, 0x0102, and 0x0104 are data.
bool BbcMachineLoadTape(BbcMachine* bbc, const char* path);
void BbcMachineSetPrinter(BbcMachine* bbc, const char* path);
size_t BbcMachinePrinterLength(const BbcMachine* bbc);
uint8_t BbcMachinePrinterByte(const BbcMachine* bbc, size_t index);
// Pull synthesized SN76489 samples (48 kHz, mono int16). Returns the count.
// volume is 0 (silent) through 11 (full). The machine starts at 7.
void BbcMachineSetVolume(BbcMachine* bbc, int volume);
int BbcMachineReadAudio(BbcMachine* bbc, int16_t* dst, int max_samples);

void BbcMachineRender(BbcMachine* bbc);
int BbcFrameWidth(const BbcMachine* bbc);
int BbcFrameHeight(const BbcMachine* bbc);
// RGB8, row-major, BBC_FB_WIDTH stride. Valid after a completed beam
// frame, or after BbcMachineRender.
const uint8_t* BbcFramebuffer(const BbcMachine* bbc);
void BbcPixel(const BbcMachine* bbc, int x, int y, uint8_t rgb[3]);
bool BbcMachineWritePpm(BbcMachine* bbc, const char* path);

// Device that claims &FC00-&FEFF. The machine is not owned by the device.
Device* BbcIoDeviceCreate(BbcMachine* bbc);

#endif
