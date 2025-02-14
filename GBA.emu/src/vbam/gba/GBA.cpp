#include <cmath>
#include <memory.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _MSC_VER
#include <strings.h>
#endif
#include "../NLS.h"
#include "../System.h"
#include "../Util.h"
#include "../common/Port.h"
#include "Cheats.h"
#include "EEprom.h"
#include "Flash.h"
#include "GBA.h"
#include "GBAGfx.h"
#include "GBALink.h"
#include "GBAcpu.h"
#include "GBAinline.h"
#include "Globals.h"
#include "Sound.h"
#include "Sram.h"
#include "agbprint.h"
#include "bios.h"
#include "elf.h"
#include "ereader.h"
#include <imagine/logger/logger.h>
#include <imagine/io/IO.hh>
#include <imagine/io/FileIO.hh>
#include <imagine/base/ApplicationContext.hh>
#include <imagine/util/algorithm.h>
#include <imagine/util/ScopeGuard.hh>
#include <emuframework/EmuSystemTaskContext.hh>

#ifdef PROFILING
#include "prof/prof.h"
#endif

#ifdef __GNUC__
#define _stricmp strcasecmp
#endif

#ifdef _MSC_VER
#define strdup _strdup
#endif

extern int emulating;
bool debugger{};

static uint8_t dummyArr[4]{};
constexpr std::array<memoryMap, 256> gbaMap = []
{
	std::array<memoryMap, 256> gbaMap
	{
		memoryMap{gGba.mem.bios, 0x3FFF , biosRead8, biosRead16, biosRead32 },
		memoryMap{dummyArr, 0, nullptr, nullptr, nullptr},
		memoryMap{gGba.mem.workRAM, 0x3FFFF, nullptr, nullptr, nullptr},
		memoryMap{gGba.mem.internalRAM, 0x7FFF, nullptr, nullptr, nullptr},
		memoryMap{gGba.mem.ioMem.b, 0x3FF , ioMemRead8, ioMemRead16, ioMemRead32},
		memoryMap{gGba.lcd.paletteRAM, 0x3FF, nullptr, nullptr, nullptr},
		memoryMap{gGba.lcd.vram, 0x1FFFF , vramRead8, vramRead16, vramRead32},
		memoryMap{gGba.lcd.oam, 0x3FF, nullptr, nullptr, nullptr},
		memoryMap{gGba.mem.rom, 0x1FFFFFF , nullptr, rtcRead16, nullptr},
		memoryMap{gGba.mem.rom, 0x1FFFFFF, nullptr, nullptr, nullptr},
		memoryMap{gGba.mem.rom, 0x1FFFFFF, nullptr, nullptr, nullptr},
		memoryMap{dummyArr, 0, nullptr, nullptr, nullptr},
		memoryMap{gGba.mem.rom, 0x1FFFFFF, nullptr, nullptr, nullptr},
		memoryMap{dummyArr, 0 , eepromRead32, eepromRead32, eepromRead32},
		memoryMap{dummyArr, 0xFFFF , flashRead32, flashRead32, flashRead32}
	};
	for(auto &m : gbaMap | std::views::drop(15)) { m = {dummyArr, 0, nullptr, nullptr, nullptr}; };
	return gbaMap;
}();

GBASys gGba;

uint32_t mastercode = 0;

int holdType = 0;
static int holdTypeDummy = 0;
bool cpuSramEnabled = true;
bool cpuFlashEnabled = true;
bool cpuEEPROMEnabled = true;
bool cpuEEPROMSensorEnabled = false;
bool saveMemoryIsMappedFile = false;

#ifdef PROFILING
int profilingTicks = 0;
int profilingTicksReload = 0;
static profile_segment* profilSegment = NULL;
#endif

#ifdef BKPT_SUPPORT
uint8_t freezeWorkRAM[SIZE_WRAM];
uint8_t freezeInternalRAM[SIZE_IRAM];
uint8_t freezeVRAM[0x18000];
uint8_t freezePRAM[SIZE_PRAM];
uint8_t freezeOAM[SIZE_OAM];
bool debugger_last;
#endif

void (*cpuSaveGameFunc)(uint32_t, uint8_t) = flashSaveDecide;

uint32_t lastTime = 0;
int count = 0;

int capture = 0;
int capturePrevious = 0;
int captureNumber = 0;
constexpr bool trackOAM = 1, oamUpdated = 1;

int armOpcodeCount = 0;
int thumbOpcodeCount = 0;

constexpr int TIMER_TICKS[4] = {
  0,
  6,
  8,
  10
};

constexpr uint8_t gamepakRamWaitState[4] = { 4, 3, 2, 8 };
constexpr uint8_t gamepakWaitState[4] =  { 4, 3, 2, 8 };
constexpr uint8_t gamepakWaitState0[2] = { 2, 1 };
constexpr uint8_t gamepakWaitState1[2] = { 4, 1 };
constexpr uint8_t gamepakWaitState2[2] = { 8, 1 };
constexpr bool isInRom [16]=
  { false, false, false, false, false, false, false, false,
    true, true, true, true, true, true, false, false };

// The videoMemoryWait constants are used to add some waitstates
// if the opcode access video memory data outside of vblank/hblank
// It seems to happen on only one ticks for each pixel.
// Not used for now (too problematic with current code).
//const uint8_t videoMemoryWait[16] =
//  {0, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0};

#ifdef WORDS_BIGENDIAN
bool cpuBiosSwapped = false;
#endif

constexpr uint32_t myROM[] = {
    0xEA000006,
    0xEA000093,
    0xEA000006,
    0x00000000,
    0x00000000,
    0x00000000,
    0xEA000088,
    0x00000000,
    0xE3A00302,
    0xE1A0F000,
    0xE92D5800,
    0xE55EC002,
    0xE28FB03C,
    0xE79BC10C,
    0xE14FB000,
    0xE92D0800,
    0xE20BB080,
    0xE38BB01F,
    0xE129F00B,
    0xE92D4004,
    0xE1A0E00F,
    0xE12FFF1C,
    0xE8BD4004,
    0xE3A0C0D3,
    0xE129F00C,
    0xE8BD0800,
    0xE169F00B,
    0xE8BD5800,
    0xE1B0F00E,
    0x0000009C,
    0x0000009C,
    0x0000009C,
    0x0000009C,
    0x000001F8,
    0x000001F0,
    0x000000AC,
    0x000000A0,
    0x000000FC,
    0x00000168,
    0xE12FFF1E,
    0xE1A03000,
    0xE1A00001,
    0xE1A01003,
    0xE2113102,
    0x42611000,
    0xE033C040,
    0x22600000,
    0xE1B02001,
    0xE15200A0,
    0x91A02082,
    0x3AFFFFFC,
    0xE1500002,
    0xE0A33003,
    0x20400002,
    0xE1320001,
    0x11A020A2,
    0x1AFFFFF9,
    0xE1A01000,
    0xE1A00003,
    0xE1B0C08C,
    0x22600000,
    0x42611000,
    0xE12FFF1E,
    0xE92D0010,
    0xE1A0C000,
    0xE3A01001,
    0xE1500001,
    0x81A000A0,
    0x81A01081,
    0x8AFFFFFB,
    0xE1A0000C,
    0xE1A04001,
    0xE3A03000,
    0xE1A02001,
    0xE15200A0,
    0x91A02082,
    0x3AFFFFFC,
    0xE1500002,
    0xE0A33003,
    0x20400002,
    0xE1320001,
    0x11A020A2,
    0x1AFFFFF9,
    0xE0811003,
    0xE1B010A1,
    0xE1510004,
    0x3AFFFFEE,
    0xE1A00004,
    0xE8BD0010,
    0xE12FFF1E,
    0xE0010090,
    0xE1A01741,
    0xE2611000,
    0xE3A030A9,
    0xE0030391,
    0xE1A03743,
    0xE2833E39,
    0xE0030391,
    0xE1A03743,
    0xE2833C09,
    0xE283301C,
    0xE0030391,
    0xE1A03743,
    0xE2833C0F,
    0xE28330B6,
    0xE0030391,
    0xE1A03743,
    0xE2833C16,
    0xE28330AA,
    0xE0030391,
    0xE1A03743,
    0xE2833A02,
    0xE2833081,
    0xE0030391,
    0xE1A03743,
    0xE2833C36,
    0xE2833051,
    0xE0030391,
    0xE1A03743,
    0xE2833CA2,
    0xE28330F9,
    0xE0000093,
    0xE1A00840,
    0xE12FFF1E,
    0xE3A00001,
    0xE3A01001,
    0xE92D4010,
    0xE3A03000,
    0xE3A04001,
    0xE3500000,
    0x1B000004,
    0xE5CC3301,
    0xEB000002,
    0x0AFFFFFC,
    0xE8BD4010,
    0xE12FFF1E,
    0xE3A0C301,
    0xE5CC3208,
    0xE15C20B8,
    0xE0110002,
    0x10222000,
    0x114C20B8,
    0xE5CC4208,
    0xE12FFF1E,
    0xE92D500F,
    0xE3A00301,
    0xE1A0E00F,
    0xE510F004,
    0xE8BD500F,
    0xE25EF004,
    0xE59FD044,
    0xE92D5000,
    0xE14FC000,
    0xE10FE000,
    0xE92D5000,
    0xE3A0C302,
    0xE5DCE09C,
    0xE35E00A5,
    0x1A000004,
    0x05DCE0B4,
    0x021EE080,
    0xE28FE004,
    0x159FF018,
    0x059FF018,
    0xE59FD018,
    0xE8BD5000,
    0xE169F00C,
    0xE8BD5000,
    0xE25EF004,
    0x03007FF0,
    0x09FE2000,
    0x09FFC000,
    0x03007FE0
};

static bool saveNFlag, saveZFlag;

static const variable_desc saveGameStruct[] = {
  { &gGba.mem.ioMem.DISPCNT  , sizeof(uint16_t) },
  { &gGba.mem.ioMem.DISPSTAT , sizeof(uint16_t) },
  { &gGba.mem.ioMem.VCOUNT   , sizeof(uint16_t) },
  { &gGba.mem.ioMem.BG0CNT   , sizeof(uint16_t) },
  { &gGba.mem.ioMem.BG1CNT   , sizeof(uint16_t) },
  { &gGba.mem.ioMem.BG2CNT   , sizeof(uint16_t) },
  { &gGba.mem.ioMem.BG3CNT   , sizeof(uint16_t) },
  { &gGba.mem.ioMem.BG0HOFS  , sizeof(uint16_t) },
  { &gGba.mem.ioMem.BG0VOFS  , sizeof(uint16_t) },
  { &gGba.mem.ioMem.BG1HOFS  , sizeof(uint16_t) },
  { &gGba.mem.ioMem.BG1VOFS  , sizeof(uint16_t) },
  { &gGba.mem.ioMem.BG2HOFS  , sizeof(uint16_t) },
  { &gGba.mem.ioMem.BG2VOFS  , sizeof(uint16_t) },
  { &gGba.mem.ioMem.BG3HOFS  , sizeof(uint16_t) },
  { &gGba.mem.ioMem.BG3VOFS  , sizeof(uint16_t) },
  { &gGba.mem.ioMem.BG2PA    , sizeof(uint16_t) },
  { &gGba.mem.ioMem.BG2PB    , sizeof(uint16_t) },
  { &gGba.mem.ioMem.BG2PC    , sizeof(uint16_t) },
  { &gGba.mem.ioMem.BG2PD    , sizeof(uint16_t) },
  { &gGba.mem.ioMem.BG2X_L   , sizeof(uint16_t) },
  { &gGba.mem.ioMem.BG2X_H   , sizeof(uint16_t) },
  { &gGba.mem.ioMem.BG2Y_L   , sizeof(uint16_t) },
  { &gGba.mem.ioMem.BG2Y_H   , sizeof(uint16_t) },
  { &gGba.mem.ioMem.BG3PA    , sizeof(uint16_t) },
  { &gGba.mem.ioMem.BG3PB    , sizeof(uint16_t) },
  { &gGba.mem.ioMem.BG3PC    , sizeof(uint16_t) },
  { &gGba.mem.ioMem.BG3PD    , sizeof(uint16_t) },
  { &gGba.mem.ioMem.BG3X_L   , sizeof(uint16_t) },
  { &gGba.mem.ioMem.BG3X_H   , sizeof(uint16_t) },
  { &gGba.mem.ioMem.BG3Y_L   , sizeof(uint16_t) },
  { &gGba.mem.ioMem.BG3Y_H   , sizeof(uint16_t) },
  { &gGba.mem.ioMem.WIN0H    , sizeof(uint16_t) },
  { &gGba.mem.ioMem.WIN1H    , sizeof(uint16_t) },
  { &gGba.mem.ioMem.WIN0V    , sizeof(uint16_t) },
  { &gGba.mem.ioMem.WIN1V    , sizeof(uint16_t) },
  { &gGba.mem.ioMem.WININ    , sizeof(uint16_t) },
  { &gGba.mem.ioMem.WINOUT   , sizeof(uint16_t) },
  { &gGba.mem.ioMem.MOSAIC   , sizeof(uint16_t) },
  { &gGba.mem.ioMem.BLDMOD   , sizeof(uint16_t) },
  { &gGba.mem.ioMem.COLEV    , sizeof(uint16_t) },
  { &gGba.mem.ioMem.COLY     , sizeof(uint16_t) },
  { &gGba.mem.ioMem.DM0SAD_L , sizeof(uint16_t) },
  { &gGba.mem.ioMem.DM0SAD_H , sizeof(uint16_t) },
  { &gGba.mem.ioMem.DM0DAD_L , sizeof(uint16_t) },
  { &gGba.mem.ioMem.DM0DAD_H , sizeof(uint16_t) },
  { &gGba.mem.ioMem.DM0CNT_L , sizeof(uint16_t) },
  { &gGba.mem.ioMem.DM0CNT_H , sizeof(uint16_t) },
  { &gGba.mem.ioMem.DM1SAD_L , sizeof(uint16_t) },
  { &gGba.mem.ioMem.DM1SAD_H , sizeof(uint16_t) },
  { &gGba.mem.ioMem.DM1DAD_L , sizeof(uint16_t) },
  { &gGba.mem.ioMem.DM1DAD_H , sizeof(uint16_t) },
  { &gGba.mem.ioMem.DM1CNT_L , sizeof(uint16_t) },
  { &gGba.mem.ioMem.DM1CNT_H , sizeof(uint16_t) },
  { &gGba.mem.ioMem.DM2SAD_L , sizeof(uint16_t) },
  { &gGba.mem.ioMem.DM2SAD_H , sizeof(uint16_t) },
  { &gGba.mem.ioMem.DM2DAD_L , sizeof(uint16_t) },
  { &gGba.mem.ioMem.DM2DAD_H , sizeof(uint16_t) },
  { &gGba.mem.ioMem.DM2CNT_L , sizeof(uint16_t) },
  { &gGba.mem.ioMem.DM2CNT_H , sizeof(uint16_t) },
  { &gGba.mem.ioMem.DM3SAD_L , sizeof(uint16_t) },
  { &gGba.mem.ioMem.DM3SAD_H , sizeof(uint16_t) },
  { &gGba.mem.ioMem.DM3DAD_L , sizeof(uint16_t) },
  { &gGba.mem.ioMem.DM3DAD_H , sizeof(uint16_t) },
  { &gGba.mem.ioMem.DM3CNT_L , sizeof(uint16_t) },
  { &gGba.mem.ioMem.DM3CNT_H , sizeof(uint16_t) },
  { &gGba.mem.ioMem.TM0D     , sizeof(uint16_t) },
  { &gGba.mem.ioMem.TM0CNT   , sizeof(uint16_t) },
  { &gGba.mem.ioMem.TM1D     , sizeof(uint16_t) },
  { &gGba.mem.ioMem.TM1CNT   , sizeof(uint16_t) },
  { &gGba.mem.ioMem.TM2D     , sizeof(uint16_t) },
  { &gGba.mem.ioMem.TM2CNT   , sizeof(uint16_t) },
  { &gGba.mem.ioMem.TM3D     , sizeof(uint16_t) },
  { &gGba.mem.ioMem.TM3CNT   , sizeof(uint16_t) },
  { &P1       , sizeof(uint16_t) },
  { &gGba.mem.ioMem.IE       , sizeof(uint16_t) },
  { &gGba.mem.ioMem.IF       , sizeof(uint16_t) },
  { &gGba.mem.ioMem.IME      , sizeof(uint16_t) },
  { &gGba.cpu.holdState, sizeof(bool) },
  { &holdTypeDummy, sizeof(int) },
  { &gGba.lcd.lcdTicks, sizeof(int) },
  { &gGba.timers.timer0On , sizeof(bool) },
  { &gGba.timers.timer0Ticks , sizeof(int) },
  { &gGba.timers.timer0Reload , sizeof(int) },
  { &gGba.timers.timer0ClockReload  , sizeof(int) },
  { &gGba.timers.timer1On , sizeof(bool) },
  { &gGba.timers.timer1Ticks , sizeof(int) },
  { &gGba.timers.timer1Reload , sizeof(int) },
  { &gGba.timers.timer1ClockReload  , sizeof(int) },
  { &gGba.timers.timer2On , sizeof(bool) },
  { &gGba.timers.timer2Ticks , sizeof(int) },
  { &gGba.timers.timer2Reload , sizeof(int) },
  { &gGba.timers.timer2ClockReload  , sizeof(int) },
  { &gGba.timers.timer3On , sizeof(bool) },
  { &gGba.timers.timer3Ticks , sizeof(int) },
  { &gGba.timers.timer3Reload , sizeof(int) },
  { &gGba.timers.timer3ClockReload  , sizeof(int) },
  { &gGba.dma.dma0Source , sizeof(uint32_t) },
  { &gGba.dma.dma0Dest , sizeof(uint32_t) },
  { &gGba.dma.dma1Source , sizeof(uint32_t) },
  { &gGba.dma.dma1Dest , sizeof(uint32_t) },
  { &gGba.dma.dma2Source , sizeof(uint32_t) },
  { &gGba.dma.dma2Dest , sizeof(uint32_t) },
  { &gGba.dma.dma3Source , sizeof(uint32_t) },
  { &gGba.dma.dma3Dest , sizeof(uint32_t) },
  { &gGba.lcd.fxOn, sizeof(bool) },
  { &gGba.lcd.windowOn, sizeof(bool) },
  { &saveNFlag , sizeof(bool) },
  { &gGba.cpu.C_FLAG , sizeof(bool) },
  { &saveZFlag , sizeof(bool) },
  { &gGba.cpu.V_FLAG , sizeof(bool) },
  { &gGba.cpu.armState , sizeof(bool) },
  { &gGba.cpu.armIrqEnable , sizeof(bool) },
  { &gGba.cpu.armNextPC , sizeof(uint32_t) },
  { &gGba.cpu.armMode , sizeof(int) },
  { &coreOptions.saveType , sizeof(int) },
  { NULL, 0 }
};

static int romSize = SIZE_ROM;

#define SWITicks cpu.SWITicks
#define IRQTicks cpu.IRQTicks
#define memoryWait cpu.memoryWait
#define memoryWait32 cpu.memoryWait32
#define memoryWaitSeq cpu.memoryWaitSeq
#define memoryWaitSeq32 cpu.memoryWaitSeq32
#define timer0On cpu.gba->timers.timer0On
#define timer0Ticks cpu.gba->timers.timer0Ticks
#define timer1On cpu.gba->timers.timer1On
#define timer1Ticks cpu.gba->timers.timer1Ticks
#define timer2On cpu.gba->timers.timer2On
#define timer2Ticks cpu.gba->timers.timer2Ticks
#define timer3On cpu.gba->timers.timer3On
#define timer3Ticks cpu.gba->timers.timer3Ticks
#define CPUReadMemory(addr) CPUReadMemory(cpu, addr)
#define CPUReadHalfWord(addr) CPUReadHalfWord(cpu, addr)

#define TM0CNT cpu.gba->mem.ioMem.TM0CNT
#define TM1CNT cpu.gba->mem.ioMem.TM1CNT
#define TM2CNT cpu.gba->mem.ioMem.TM2CNT
#define TM3CNT cpu.gba->mem.ioMem.TM3CNT
#define DM0CNT_H gba.mem.ioMem.DM0CNT_H
#define DM1CNT_H gba.mem.ioMem.DM1CNT_H
#define DM2CNT_H gba.mem.ioMem.DM2CNT_H
#define DM3CNT_H gba.mem.ioMem.DM3CNT_H
#define BG0CNT gba.mem.ioMem.BG0CNT
#define BG1CNT gba.mem.ioMem.BG1CNT
#define BG2CNT gba.mem.ioMem.BG2CNT
#define BG3CNT gba.mem.ioMem.BG3CNT
#define BG0HOFS gba.mem.ioMem.BG0HOFS
#define BG0VOFS gba.mem.ioMem.BG0VOFS
#define BG1HOFS gba.mem.ioMem.BG1HOFS
#define BG1VOFS gba.mem.ioMem.BG1VOFS
#define BG2HOFS gba.mem.ioMem.BG2HOFS
#define BG2VOFS gba.mem.ioMem.BG2VOFS
#define BG3HOFS gba.mem.ioMem.BG3HOFS
#define BG3VOFS gba.mem.ioMem.BG3VOFS
#define DISPCNT gba.mem.ioMem.DISPCNT
#define BG2PA gba.mem.ioMem.BG2PA
#define BG2PB gba.mem.ioMem.BG2PB
#define BG2PC gba.mem.ioMem.BG2PC
#define BG2PD gba.mem.ioMem.BG2PD
#define BG2X_L gba.mem.ioMem.BG2X_L
#define BG2X_H gba.mem.ioMem.BG2X_H
#define BG2Y_L gba.mem.ioMem.BG2Y_L
#define BG2Y_H gba.mem.ioMem.BG2Y_H
#define BG3PA gba.mem.ioMem.BG3PA
#define BG3PB gba.mem.ioMem.BG3PB
#define BG3PC gba.mem.ioMem.BG3PC
#define BG3PD gba.mem.ioMem.BG3PD
#define BG3X_L gba.mem.ioMem.BG3X_L
#define BG3X_H gba.mem.ioMem.BG3X_H
#define BG3Y_L gba.mem.ioMem.BG3Y_L
#define BG3Y_H gba.mem.ioMem.BG3Y_H
#define WIN0H gba.mem.ioMem.WIN0H
#define WIN1H gba.mem.ioMem.WIN1H
#define WIN0V gba.mem.ioMem.WIN0V
#define WIN1V gba.mem.ioMem.WIN1V
#define WININ gba.mem.ioMem.WININ
#define WINOUT gba.mem.ioMem.WINOUT
#define MOSAIC gba.mem.ioMem.MOSAIC
#define BLDMOD gba.mem.ioMem.BLDMOD
#define COLEV gba.mem.ioMem.COLEV
#define COLY gba.mem.ioMem.COLY
#define DM0SAD_L gba.mem.ioMem.DM0SAD_L
#define DM0SAD_H gba.mem.ioMem.DM0SAD_H
#define DM0DAD_L gba.mem.ioMem.DM0DAD_L
#define DM0DAD_H gba.mem.ioMem.DM0DAD_H
#define DM1SAD_L gba.mem.ioMem.DM1SAD_L
#define DM1SAD_H gba.mem.ioMem.DM1SAD_H
#define DM1DAD_L gba.mem.ioMem.DM1DAD_L
#define DM1DAD_H gba.mem.ioMem.DM1DAD_H
#define DM2SAD_L gba.mem.ioMem.DM2SAD_L
#define DM2SAD_H gba.mem.ioMem.DM2SAD_H
#define DM2DAD_L gba.mem.ioMem.DM2DAD_L
#define DM2DAD_H gba.mem.ioMem.DM2DAD_H
#define DM3SAD_L gba.mem.ioMem.DM3SAD_L
#define DM3SAD_H gba.mem.ioMem.DM3SAD_H
#define DM3DAD_L gba.mem.ioMem.DM3DAD_L
#define DM3DAD_H gba.mem.ioMem.DM3DAD_H
#define DM0CNT_L gba.mem.ioMem.DM0CNT_L
#define DM1CNT_L gba.mem.ioMem.DM1CNT_L
#define DM2CNT_L gba.mem.ioMem.DM2CNT_L
#define DM3CNT_L gba.mem.ioMem.DM3CNT_L
#define TM0D gba.mem.ioMem.TM0D
#define TM1D gba.mem.ioMem.TM1D
#define TM2D gba.mem.ioMem.TM2D
#define TM3D gba.mem.ioMem.TM3D
#define DISPSTAT gba.mem.ioMem.DISPSTAT
#define VCOUNT gba.mem.ioMem.VCOUNT
#define layerEnableDelay gba.lcd.layerEnableDelay
#define layerEnable gba.lcd.layerEnable
#define windowOn gba.lcd.windowOn
#define gfxBG2Changed gba.lcd.gfxBG2Changed
#define gfxBG3Changed gba.lcd.gfxBG3Changed
#define gfxInWin0 gba.lcd.gfxInWin0
#define gfxInWin1 gba.lcd.gfxInWin1
#define fxOn gba.lcd.fxOn
#define bios gba.mem.bios
#define cpuDmaCount gba.dma.cpuDmaCount
#define internalRAM gba.mem.internalRAM
#define paletteRAM gba.lcd.paletteRAM
#define workRAM gba.mem.workRAM
#define rom gba.mem.rom
#define vram gba.lcd.vram
#define oam gba.lcd.oam
#define biosProtected gba.biosProtected

static void UPDATE_REG(auto, auto) {} // dummy function

#if 0
void gbaUpdateRomSize(int size)
{
    // Only change memory block if new size is larger
    if (size > romSize) {
        romSize = size;

        uint8_t* tmp = (uint8_t*)realloc(rom, SIZE_ROM);
        rom = tmp;

        uint16_t* temp = (uint16_t*)(rom + ((romSize + 1) & ~1));
        for (int i = (romSize + 1) & ~1; i < SIZE_ROM; i += 2) {
            WRITE16LE(temp, (i >> 1) & 0xFFFF);
            temp++;
        }
    }
}
#endif

#ifdef PROFILING
void cpuProfil(profile_segment* seg)
{
    profilSegment = seg;
}

void cpuEnableProfiling(int hz)
{
    if (hz == 0)
        hz = 100;
    profilingTicks = profilingTicksReload = 16777216 / hz;
    profSetHertz(hz);
}
#endif

inline int CPUUpdateTicks(ARM7TDMI &cpu)
{
  int cpuLoopTicks = cpu.gba->lcd.lcdTicks;

  //if (soundTicks < cpuLoopTicks)
      //cpuLoopTicks = soundTicks;

  if (timer0On && (timer0Ticks < cpuLoopTicks)) {
      cpuLoopTicks = timer0Ticks;
  }
  if (timer1On && !(TM1CNT & 4) && (timer1Ticks < cpuLoopTicks)) {
      cpuLoopTicks = timer1Ticks;
  }
  if (timer2On && !(TM2CNT & 4) && (timer2Ticks < cpuLoopTicks)) {
      cpuLoopTicks = timer2Ticks;
  }
  if (timer3On && !(TM3CNT & 4) && (timer3Ticks < cpuLoopTicks)) {
      cpuLoopTicks = timer3Ticks;
  }
#ifdef PROFILING
    if (profilingTicksReload != 0) {
        if (profilingTicks < cpuLoopTicks) {
            cpuLoopTicks = profilingTicks;
        }
    }
#endif

  if (SWITicks) {
    if (SWITicks < cpuLoopTicks)
        cpuLoopTicks = SWITicks;
  }

  if (IRQTicks) {
    if (IRQTicks < cpuLoopTicks)
        cpuLoopTicks = IRQTicks;
  }

  return cpuLoopTicks;
}

static void CPUUpdateWindow0(GBASys &gba)
{
  int x00 = WIN0H >> 8;
  int x01 = WIN0H & 255;

  if (x00 <= x01) {
    for (int i = 0; i < 240; i++) {
      gfxInWin0[i] = (i >= x00 && i < x01);
    }
  } else {
    for (int i = 0; i < 240; i++) {
    	gfxInWin0[i] = (i >= x00 || i < x01);
    }
  }
}

static void CPUUpdateWindow1(GBASys &gba)
{
  int x00 = WIN1H >> 8;
  int x01 = WIN1H & 255;

  if (x00 <= x01) {
    for (int i = 0; i < 240; i++) {
    	gfxInWin1[i] = (i >= x00 && i < x01);
    }
  } else {
    for (int i = 0; i < 240; i++) {
    	gfxInWin1[i] = (i >= x00 || i < x01);
    }
  }
}

#define CPUUpdateTicks() CPUUpdateTicks(cpu)
#define CPUUpdateWindow0() CPUUpdateWindow0(gba)
#define CPUUpdateWindow1() CPUUpdateWindow1(gba)
#define line0 gba.lcd.line0
#define line1 gba.lcd.line1
#define line2 gba.lcd.line2
#define line3 gba.lcd.line3
#define CLEAR_ARRAY gfxClearArray
#define stopState gba.stopState
#define intState gba.intState
#define dma0Source gba.dma.dma0Source
#define dma1Source gba.dma.dma1Source
#define dma2Source gba.dma.dma2Source
#define dma3Source gba.dma.dma3Source
#define dma0Dest gba.dma.dma0Dest
#define dma1Dest gba.dma.dma1Dest
#define dma2Dest gba.dma.dma2Dest
#define dma3Dest gba.dma.dma3Dest
#define timer0Value gba.timers.timer0Value
#define timer0ClockReload gba.timers.timer0ClockReload
#define timer0Reload gba.timers.timer0Reload
#define timer1Value gba.timers.timer1Value
#define timer1ClockReload gba.timers.timer1ClockReload
#define timer1Reload gba.timers.timer1Reload
#define timer2Value gba.timers.timer2Value
#define timer2ClockReload gba.timers.timer2ClockReload
#define timer2Reload gba.timers.timer2Reload
#define timer3Value gba.timers.timer3Value
#define timer3ClockReload gba.timers.timer3ClockReload
#define timer3Reload gba.timers.timer3Reload

static void CPUUpdateRenderBuffers(GBASys &gba, bool force)
{
  if (!(layerEnable & 0x0100) || force) {
    CLEAR_ARRAY(line0);
  }
  if (!(layerEnable & 0x0200) || force) {
  	CLEAR_ARRAY(line1);
  }
  if (!(layerEnable & 0x0400) || force) {
  	CLEAR_ARRAY(line2);
  }
  if (!(layerEnable & 0x0800) || force) {
  	CLEAR_ARRAY(line3);
  }
}

#define CPUUpdateRenderBuffers(force) CPUUpdateRenderBuffers(gba, force)

#ifdef __LIBRETRO__
#include <stddef.h>

unsigned int CPUWriteState(uint8_t* data)
{
    uint8_t* orig = data;

    utilWriteIntMem(data, SAVE_GAME_VERSION);
    utilWriteMem(data, &rom[0xa0], 16);
    utilWriteIntMem(data, coreOptions.useBios);
    utilWriteMem(data, &reg[0], sizeof(reg));

    utilWriteDataMem(data, saveGameStruct);

    utilWriteIntMem(data, stopState);
    utilWriteIntMem(data, IRQTicks);

    utilWriteMem(data, internalRAM, SIZE_IRAM);
    utilWriteMem(data, paletteRAM, SIZE_PRAM);
    utilWriteMem(data, workRAM, SIZE_WRAM);
    utilWriteMem(data, vram, SIZE_VRAM);
    utilWriteMem(data, oam, SIZE_OAM);
    utilWriteMem(data, pix, SIZE_PIX);
    utilWriteMem(data, ioMem, SIZE_IOMEM);

    eepromSaveGame(data);
    flashSaveGame(data);
    soundSaveGame(data);
    rtcSaveGame(data);

    return (ptrdiff_t)data - (ptrdiff_t)orig;
}

bool CPUReadState(const uint8_t* data)
{
    // Don't really care about version.
    int version = utilReadIntMem(data);
    if (version != SAVE_GAME_VERSION)
        return false;

    char romname[16];
    utilReadMem(romname, data, 16);
    if (memcmp(&rom[0xa0], romname, 16) != 0)
        return false;

    // Don't care about use bios ...
    utilReadIntMem(data);

    utilReadMem(&reg[0], data, sizeof(reg));

    utilReadDataMem(data, saveGameStruct);

    stopState = utilReadIntMem(data) ? true : false;

    IRQTicks = utilReadIntMem(data);
    if (IRQTicks > 0)
        intState = true;
    else {
        intState = false;
        IRQTicks = 0;
    }

    utilReadMem(internalRAM, data, SIZE_IRAM);
    utilReadMem(paletteRAM, data, SIZE_PRAM);
    utilReadMem(workRAM, data, SIZE_WRAM);
    utilReadMem(vram, data, SIZE_VRAM);
    utilReadMem(oam, data, SIZE_OAM);
    utilReadMem(pix, data, SIZE_PIX);
    utilReadMem(ioMem, data, SIZE_IOMEM);

    eepromReadGame(data);
    flashReadGame(data);
    soundReadGame(data);
    rtcReadGame(data);

    //// Copypasta stuff ...
    // set pointers!
    layerEnable = coreOptions.layerSettings & DISPCNT;

    CPUUpdateRender();

    // CPU Update Render Buffers set to true
    CLEAR_ARRAY(line0);
    CLEAR_ARRAY(line1);
    CLEAR_ARRAY(line2);
    CLEAR_ARRAY(line3);
    // End of CPU Update Render Buffers set to true

    CPUUpdateWindow0();
    CPUUpdateWindow1();

    SetSaveType(coreOptions.saveType);

    systemSaveUpdateCounter = SYSTEM_SAVE_NOT_UPDATED;
    if (armState) {
        ARM_PREFETCH;
    } else {
        THUMB_PREFETCH;
    }

    CPUUpdateRegister(0x204, CPUReadHalfWordQuick(0x4000204));

    return true;
}

#else // !__LIBRETRO__

static bool CPUWriteState(GBASys &gba, gzFile gzFile)
{
	auto &cpu = gba.cpu;
  utilWriteInt(gzFile, SAVE_GAME_VERSION);

  utilGzWrite(gzFile, &rom[0xa0], 16);

  utilWriteInt(gzFile, coreOptions.useBios);

  utilGzWrite(gzFile, &gba.cpu.reg[0], sizeof(gba.cpu.reg));

  saveNFlag = gba.cpu.nFlag();
  saveZFlag = gba.cpu.zFlag();
  utilWriteData(gzFile, saveGameStruct);

  // new to version 0.7.1
  utilWriteInt(gzFile, stopState);
  // new to version 0.8
#ifdef VBAM_USE_IRQTICKS
  utilWriteInt(gzFile, IRQTicks);
#else
  int dummyIRQTicks = 0;
  utilWriteInt(gzFile, dummyIRQTicks);
#endif

  utilGzWrite(gzFile, internalRAM, SIZE_IRAM);
  utilGzWrite(gzFile, paletteRAM, SIZE_PRAM);
  utilGzWrite(gzFile, workRAM, SIZE_WRAM);
  utilGzWrite(gzFile, vram, SIZE_VRAM);
  utilGzWrite(gzFile, oam, SIZE_OAM);
  uint32_t dummyPix[241*162]{};
  utilGzWrite(gzFile, dummyPix, 4*241*162);
  utilGzWrite(gzFile, gba.mem.ioMem.b, SIZE_IOMEM);

  eepromSaveGame(gzFile);
  flashSaveGame(gzFile);
  soundSaveGame(gzFile);

  cheatsSaveGame(gzFile);

  // version 1.5
  rtcSaveGame(gzFile);

  return true;
}

bool CPUWriteState(IG::ApplicationContext ctx, GBASys &gba, const char* file)
{
  gzFile gzFile = utilGzOpen(ctx.openFileUriFd(file, IG::OpenFlags::testNewFile()).release(), "wb");

  if (gzFile == NULL) {
    systemMessage(MSG_ERROR_CREATING_FILE, N_("Error creating file %s"), file);
    return false;
  }

  bool res = CPUWriteState(gba, gzFile);

  utilGzClose(gzFile);

  return res;
}

bool CPUWriteMemState(GBASys &gba, char *memory, int available, long& reserved)
{
  gzFile gzFile = utilMemGzOpen(memory, available, "w");

  if (gzFile == NULL) {
    return false;
  }

  bool res = CPUWriteState(gba, gzFile);

  reserved = utilGzMemTell(gzFile)+8;

  if (reserved >= (available))
    res = false;

  utilGzClose(gzFile);

  return res;
}

static bool CPUReadState(GBASys &gba, gzFile gzFile)
{
	auto &cpu = gba.cpu;
  int version = utilReadInt(gzFile);

  if (version > SAVE_GAME_VERSION || version < SAVE_GAME_VERSION_1) {
    systemMessage(MSG_UNSUPPORTED_VBA_SGM,
                  N_("Unsupported VisualBoyAdvance save game version %d"),
                  version);
    return false;
  }

  uint8_t romname[17];

  utilGzRead(gzFile, romname, 16);

  if (memcmp(&rom[0xa0], romname, 16) != 0) {
    romname[16] = 0;
    for (int i = 0; i < 16; i++)
      if (romname[i] < 32)
        romname[i] = 32;
    systemMessage(MSG_CANNOT_LOAD_SGM, N_("Cannot load save game for %s"), romname);
    return false;
  }

  bool ub = utilReadInt(gzFile) ? true : false;

  if (ub != coreOptions.useBios) {
    if (coreOptions.useBios)
      systemMessage(MSG_SAVE_GAME_NOT_USING_BIOS,
                    N_("Save game is not using the BIOS files"));
    else
      systemMessage(MSG_SAVE_GAME_USING_BIOS,
                    N_("Save game is using the BIOS file"));
    return false;
  }

  utilGzRead(gzFile, &gba.cpu.reg[0], sizeof(gba.cpu.reg));

  auto backupSaveType = coreOptions.saveType;
  utilReadData(gzFile, saveGameStruct);
  if(backupSaveType != coreOptions.saveType)
  {
    systemMessage(0, "Save type:%d in state was corrected to:%d", coreOptions.saveType, backupSaveType);
    coreOptions.saveType = backupSaveType;
  }
  gba.cpu.updateNZFlags(saveNFlag, saveZFlag);

  if (version < SAVE_GAME_VERSION_3)
  	stopState = false;
  else
  	stopState = utilReadInt(gzFile) ? true : false;

  if (version < SAVE_GAME_VERSION_4) {
  	IRQTicks = 0;
  	intState = false;
  } else {
  	IRQTicks = utilReadInt(gzFile);
    if (IRQTicks > 0)
      intState = true;
    else {
      intState = false;
      IRQTicks = 0;
    }
  }

  utilGzRead(gzFile, internalRAM, SIZE_IRAM);
  utilGzRead(gzFile, paletteRAM, SIZE_PRAM);
  utilGzRead(gzFile, workRAM, SIZE_WRAM);
  utilGzRead(gzFile, vram, SIZE_VRAM);
  utilGzRead(gzFile, oam, SIZE_OAM);
  uint32_t dummyPix[241*162];
  if (version < SAVE_GAME_VERSION_6)
    utilGzRead(gzFile, dummyPix, 4*240*160);
  else
    utilGzRead(gzFile, dummyPix, 4*241*162);
  utilGzRead(gzFile, gba.mem.ioMem.b, SIZE_IOMEM);

  if (coreOptions.skipSaveGameBattery) {
    // skip eeprom data
    eepromReadGameSkip(gzFile, version);
    // skip flash data
    flashReadGameSkip(gzFile, version);
  } else {
    eepromReadGame(gzFile, version);
    flashReadGame(gzFile, version);
  }
  soundReadGame(gba, gzFile, version);

  if (version > SAVE_GAME_VERSION_1) {
    if (coreOptions.skipSaveGameCheats) {
      // skip cheats list data
      cheatsReadGameSkip(gzFile, version);
    } else {
      cheatsReadGame(gzFile, version);
    }
  }
  if (version > SAVE_GAME_VERSION_6) {
    rtcReadGame(gzFile);
  }

  if (version <= SAVE_GAME_VERSION_7) {
    uint32_t temp;
#define SWAP(a, b, c)      \
    temp = (a);            \
    (a) = (b) << 16 | (c); \
    (b) = (temp) >> 16;    \
    (c) = (temp)&0xFFFF;

    SWAP(dma0Source, DM0SAD_H, DM0SAD_L);
    SWAP(dma0Dest,   DM0DAD_H, DM0DAD_L);
    SWAP(dma1Source, DM1SAD_H, DM1SAD_L);
    SWAP(dma1Dest,   DM1DAD_H, DM1DAD_L);
    SWAP(dma2Source, DM2SAD_H, DM2SAD_L);
    SWAP(dma2Dest,   DM2DAD_H, DM2DAD_L);
    SWAP(dma3Source, DM3SAD_H, DM3SAD_L);
    SWAP(dma3Dest,   DM3DAD_H, DM3DAD_L);
  }

  if (version <= SAVE_GAME_VERSION_8) {
  	timer0ClockReload = TIMER_TICKS[TM0CNT & 3];
  	timer1ClockReload = TIMER_TICKS[TM1CNT & 3];
  	timer2ClockReload = TIMER_TICKS[TM2CNT & 3];
  	timer3ClockReload = TIMER_TICKS[TM3CNT & 3];

  	timer0Ticks = ((0x10000 - TM0D) << timer0ClockReload) - timer0Ticks;
  	timer1Ticks = ((0x10000 - TM1D) << timer1ClockReload) - timer1Ticks;
  	timer2Ticks = ((0x10000 - TM2D) << timer2ClockReload) - timer2Ticks;
  	timer3Ticks = ((0x10000 - TM3D) << timer3ClockReload) - timer3Ticks;
    interp_rate();
  }

  // set pointers!
  layerEnable = coreOptions.layerSettings & DISPCNT;

  CPUUpdateRender(gba);
  CPUUpdateRenderBuffers(true);
  CPUUpdateWindow0();
  CPUUpdateWindow1();

  SetSaveType(coreOptions.saveType);

  systemSaveUpdateCounter = SYSTEM_SAVE_NOT_UPDATED;
  if (gba.cpu.armState) {
  	gba.cpu.ARM_PREFETCH();
  } else {
  	gba.cpu.THUMB_PREFETCH();
  }

  CPUUpdateRegister(gba.cpu, 0x204, CPUReadHalfWordQuick(gba.cpu, 0x4000204));

  return true;
}

bool CPUReadMemState(GBASys &gba, char *memory, int available)
{
  gzFile gzFile = utilMemGzOpen(memory, available, "r");

  bool res = CPUReadState(gba, gzFile);

  utilGzClose(gzFile);

  return res;
}

bool CPUReadState(IG::ApplicationContext ctx, GBASys &gba, const char * file)
{
  gzFile gzFile = utilGzOpen(ctx.openFileUriFd(file).release(), "rb");

  if (gzFile == NULL)
    return false;

  bool res = CPUReadState(gba, gzFile);

  utilGzClose(gzFile);

  return res;
}
#endif

bool CPUExportEepromFile(const char* fileName)
{
  if (eepromInUse) {
    FILE* file = fopen(fileName, "wb");

    if (!file) {
      systemMessage(MSG_ERROR_CREATING_FILE, N_("Error creating file %s"),
                    fileName);
      return false;
    }

    for (int i = 0; i < eepromSize;) {
      for (int j = 0; j < 8; j++) {
        if (fwrite(&eepromData[i + 7 - j], 1, 1, file) != 1) {
          fclose(file);
          return false;
        }
      }
      i += 8;
    }
    fclose(file);
  }
  return true;
}

bool CPUWriteBatteryFile(IG::ApplicationContext ctx, GBASys &gba, const char* fileName)
{
  if(saveMemoryIsMappedFile)
    return false;
  if ((coreOptions.saveType) && (coreOptions.saveType != GBA_SAVE_NONE)) {
    FILE* file = IG::FileUtils::fopenUri(ctx, fileName, "wb");

    if (!file) {
      systemMessage(MSG_ERROR_CREATING_FILE, N_("Error creating file %s"),
                    fileName);
      return false;
    }

    // only save if Flash/Sram in use or EEprom in use
    if (!eepromInUse) {
    	if (coreOptions.saveType == GBA_SAVE_FLASH) { // save flash type
        if (fwrite(flashSaveMemory.data(), 1, flashSize, file) != (size_t)flashSize) {
          fclose(file);
          return false;
        }
    	} else if (coreOptions.saveType == GBA_SAVE_SRAM) { // save sram type
        if (fwrite(flashSaveMemory.data(), 1, 0x8000, file) != 0x8000) {
          fclose(file);
          return false;
        }
      }
    } else { // save eeprom type
      if (fwrite(eepromData.data(), 1, eepromSize, file) != (size_t)eepromSize) {
        fclose(file);
        return false;
      }
    }
    fclose(file);
  }
  return true;
}

bool CPUReadGSASnapshot(GBASys &gba, const char* fileName)
{
  int i;
  FILE* file = fopen(fileName, "rb");

  if (!file) {
    systemMessage(MSG_CANNOT_OPEN_FILE, N_("Cannot open file %s"), fileName);
    return false;
  }

  // check file size to know what we should read
  fseek(file, 0, SEEK_END);

  // long size = ftell(file);
  fseek(file, 0x0, SEEK_SET);
  FREAD_UNCHECKED(&i, 1, 4, file);
  fseek(file, i, SEEK_CUR); // Skip SharkPortSave
  fseek(file, 4, SEEK_CUR); // skip some sort of flag
  FREAD_UNCHECKED(&i, 1, 4, file); // name length
  fseek(file, i, SEEK_CUR); // skip name
  FREAD_UNCHECKED(&i, 1, 4, file); // desc length
  fseek(file, i, SEEK_CUR); // skip desc
  FREAD_UNCHECKED(&i, 1, 4, file); // notes length
  fseek(file, i, SEEK_CUR); // skip notes
  int saveSize;
  FREAD_UNCHECKED(&saveSize, 1, 4, file); // read length
  saveSize -= 0x1c; // remove header size
  char buffer[17];
  char buffer2[17];
  FREAD_UNCHECKED(buffer, 1, 16, file);
  buffer[16] = 0;
  for (i = 0; i < 16; i++)
    if (buffer[i] < 32)
      buffer[i] = 32;
  memcpy(buffer2, &rom[0xa0], 16);
  buffer2[16] = 0;
  for (i = 0; i < 16; i++)
    if (buffer2[i] < 32)
      buffer2[i] = 32;
  if (memcmp(buffer, buffer2, 16)) {
    systemMessage(MSG_CANNOT_IMPORT_SNAPSHOT_FOR,
                  N_("Cannot import snapshot for %s. Current game is %s"),
                  buffer,
                  buffer2);
    fclose(file);
    return false;
  }
  fseek(file, 12, SEEK_CUR); // skip some flags
  if (saveSize >= 65536) {
    if (fread(flashSaveMemory.data(), 1, saveSize, file) != (size_t)saveSize) {
      fclose(file);
      return false;
    }
  } else {
    systemMessage(MSG_UNSUPPORTED_SNAPSHOT_FILE,
                  N_("Unsupported snapshot file %s"),
                  fileName);
    fclose(file);
    return false;
  }
  fclose(file);
  CPUReset(gba);
  return true;
}

bool CPUReadGSASPSnapshot(GBASys &gba, const char* fileName)
{
  const char gsvfooter[] = "xV4\x12";
  const size_t namepos = 0x0c, namesz = 12;
  const size_t footerpos = 0x42c, footersz = 4;

  char footer[footersz + 1], romname[namesz + 1], savename[namesz + 1];
  FILE* file = fopen(fileName, "rb");

  if (!file) {
    systemMessage(MSG_CANNOT_OPEN_FILE, N_("Cannot open file %s"), fileName);
    return false;
  }

  // read save name
  fseek(file, namepos, SEEK_SET);
  FREAD_UNCHECKED(savename, 1, namesz, file);
  savename[namesz] = 0;

  memcpy(romname, &rom[0xa0], namesz);
  romname[namesz] = 0;

  if (memcmp(romname, savename, namesz)) {
    systemMessage(MSG_CANNOT_IMPORT_SNAPSHOT_FOR,
                  N_("Cannot import snapshot for %s. Current game is %s"),
                  savename,
                  romname);
    fclose(file);
    return false;
  }

  // read footer tag
  fseek(file, footerpos, SEEK_SET);
  FREAD_UNCHECKED(footer, 1, footersz, file);
  footer[footersz] = 0;

  if (memcmp(footer, gsvfooter, footersz)) {
    systemMessage(0,
                  N_("Unsupported snapshot file %s. Footer '%s' at %u should be '%s'"),
                  fileName,
				  footer,
                  footerpos,
				  gsvfooter);
    fclose(file);
    return false;
  }

  // Read up to 128k save
  FREAD_UNCHECKED(flashSaveMemory.data(), 1, FLASH_128K_SZ, file);

  fclose(file);
  CPUReset(gba);
  return true;
}


bool CPUWriteGSASnapshot(GBASys &gba, const char* fileName,
                         const char* title,
                         const char* desc,
                         const char* notes)
{
  FILE* file = fopen(fileName, "wb");

  if (!file) {
    systemMessage(MSG_CANNOT_OPEN_FILE, N_("Cannot open file %s"), fileName);
    return false;
  }

  uint8_t buffer[17];

  utilPutDword(buffer, 0x0d); // SharkPortSave length
  fwrite(buffer, 1, 4, file);
  fwrite("SharkPortSave", 1, 0x0d, file);
  utilPutDword(buffer, 0x000f0000);
  fwrite(buffer, 1, 4, file); // save type 0x000f0000 = GBA save
  utilPutDword(buffer, (uint32_t)strlen(title));
  fwrite(buffer, 1, 4, file); // title length
  fwrite(title, 1, strlen(title), file);
  utilPutDword(buffer, (uint32_t)strlen(desc));
  fwrite(buffer, 1, 4, file); // desc length
  fwrite(desc, 1, strlen(desc), file);
  utilPutDword(buffer, (uint32_t)strlen(notes));
  fwrite(buffer, 1, 4, file); // notes length
  fwrite(notes, 1, strlen(notes), file);
  int saveSize = 0x10000;
  if (coreOptions.saveType == GBA_SAVE_FLASH)
    saveSize = flashSize;
  int totalSize = saveSize + 0x1c;

  utilPutDword(buffer, totalSize); // length of remainder of save - CRC
  fwrite(buffer, 1, 4, file);

  char* temp = new char[0x2001c];
  memset(temp, 0, 28);
  memcpy(temp, &rom[0xa0], 16); // copy internal name
  temp[0x10] = rom[0xbe]; // reserved area (old checksum)
  temp[0x11] = rom[0xbf]; // reserved area (old checksum)
  temp[0x12] = rom[0xbd]; // complement check
  temp[0x13] = rom[0xb0]; // maker
  temp[0x14] = 1; // 1 save ?
  memcpy(&temp[0x1c], flashSaveMemory.data(), saveSize); // copy save
  fwrite(temp, 1, totalSize, file); // write save + header
  uint32_t crc = 0;

  for (int i = 0; i < totalSize; i++) {
    crc += ((uint32_t)temp[i] << (crc % 0x18));
  }

  utilPutDword(buffer, crc);
  fwrite(buffer, 1, 4, file); // CRC?

  fclose(file);
  delete[] temp;
  return true;
}

bool CPUImportEepromFile(GBASys &gba, const char* fileName)
{
  FILE* file = fopen(fileName, "rb");

  if (!file)
    return false;

  // check file size to know what we should read
  fseek(file, 0, SEEK_END);

  long size = ftell(file);
  fseek(file, 0, SEEK_SET);
  if (size == 512 || size == 0x2000) {
    if (fread(eepromData.data(), 1, size, file) != (size_t)size) {
      fclose(file);
      return false;
    }
    for (int i = 0; i < size;) {
      uint8_t tmp = eepromData[i];
      eepromData[i] = eepromData[7 - i];
      eepromData[7 - i] = tmp;
      i++;
      tmp = eepromData[i];
      eepromData[i] = eepromData[7 - i];
      eepromData[7 - i] = tmp;
      i++;
      tmp = eepromData[i];
      eepromData[i] = eepromData[7 - i];
      eepromData[7 - i] = tmp;
      i++;
      tmp = eepromData[i];
      eepromData[i] = eepromData[7 - i];
      eepromData[7 - i] = tmp;
      i++;
      i += 4;
    }
  } else {
    fclose(file);
    return false;
  }
  fclose(file);
  return true;
}

bool CPUReadBatteryFile(IG::ApplicationContext ctx, GBASys &gba, const char* fileName)
{
  auto buff = IG::FileUtils::rwBufferFromUri(ctx, fileName, {.test = true}, saveMemorySize(), 0xFF);
  if(!buff)
    return false;
  saveMemoryIsMappedFile = buff.isMappedFile();
  setSaveMemory(std::move(buff));
  return true;
}

#if 0
bool CPUWritePNGFile(const char* fileName)
{
  return utilWritePNGFile(fileName, 240, 160, pix);
}

bool CPUWriteBMPFile(const char* fileName)
{
  return utilWriteBMPFile(fileName, 240, 160, pix);
}
#endif

bool CPUIsZipFile(const char* file)
{
  if (strlen(file) > 4) {
    const char* p = strrchr(file, '.');

    if (p != NULL) {
      if (_stricmp(p, ".zip") == 0)
        return true;
    }
  }

  return false;
}

bool CPUIsGBAImage(const char* file)
{
  coreOptions.cpuIsMultiBoot = false;
  if (strlen(file) > 4) {
    const char* p = strrchr(file, '.');

    if (p != NULL) {
      if (_stricmp(p, ".gba") == 0)
        return true;
      if (_stricmp(p, ".agb") == 0)
        return true;
      if (_stricmp(p, ".bin") == 0)
        return true;
      if (_stricmp(p, ".elf") == 0)
        return true;
      if (_stricmp(p, ".mb") == 0) {
        coreOptions.cpuIsMultiBoot = true;
        return true;
      }
    }
  }

  return false;
}

bool CPUIsGBABios(const char* file)
{
  if (strlen(file) > 4) {
    const char* p = strrchr(file, '.');

    if (p != NULL) {
      if (_stricmp(p, ".gba") == 0)
        return true;
      if (_stricmp(p, ".agb") == 0)
        return true;
      if (_stricmp(p, ".bin") == 0)
        return true;
      if (_stricmp(p, ".bios") == 0)
        return true;
      if (_stricmp(p, ".rom") == 0)
        return true;
    }
  }

  return false;
}

bool CPUIsELF(const char* file)
{
  if (file == NULL)
	  return false;

  if (strlen(file) > 4) {
    const char* p = strrchr(file, '.');

    if (p != NULL) {
      if (_stricmp(p, ".elf") == 0)
        return true;
    }
  }
  return false;
}

void CPUCleanUp()
{
#ifdef PROFILING
  if (profilingTicksReload) {
    profCleanup();
  }
#endif
#if 0
  if (rom != NULL) {
      free(rom);
      rom = NULL;
  }

  if (vram != NULL) {
      free(vram);
      vram = NULL;
  }

  if (paletteRAM != NULL) {
      free(paletteRAM);
      paletteRAM = NULL;
  }

  if (internalRAM != NULL) {
      free(internalRAM);
      internalRAM = NULL;
  }

  if (workRAM != NULL) {
      free(workRAM);
      workRAM = NULL;
  }

  if (bios != NULL) {
      free(bios);
      bios = NULL;
  }

  if (pix != NULL) {
      free(pix);
      pix = NULL;
  }

  if (oam != NULL) {
      free(oam);
      oam = NULL;
  }

  if (ioMem != NULL) {
      free(ioMem);
      ioMem = NULL;
  }
#endif
#ifndef NO_DEBUGGER
  elfCleanUp();
#endif //NO_DEBUGGER

  systemSaveUpdateCounter = SYSTEM_SAVE_NOT_UPDATED;

  flashSaveMemory = {};
  eepromData = {};
  saveMemoryIsMappedFile = false;

  emulating = 0;
}

#if 0
void SetMapMasks()
{
    map[0].mask = 0x3FFF;
    map[2].mask = 0x3FFFF;
    map[3].mask = 0x7FFF;
    map[4].mask = 0x3FF;
    map[5].mask = 0x3FF;
    map[6].mask = 0x1FFFF;
    map[7].mask = 0x3FF;
    map[8].mask = 0x1FFFFFF;
    map[9].mask = 0x1FFFFFF;
    map[10].mask = 0x1FFFFFF;
    map[12].mask = 0x1FFFFFF;
    map[14].mask = 0xFFFF;

#ifdef BKPT_SUPPORT
    for (int i = 0; i < 16; i++) {
        map[i].size = map[i].mask + 1;
        map[i].trace = NULL;
        map[i].breakPoints = NULL;

        if ((map[i].size >> 1) > 0) {
            map[i].breakPoints = (uint8_t*)calloc(map[i].size >> 1, sizeof(uint8_t));
            if (map[i].breakPoints == NULL) {
                systemMessage(MSG_OUT_OF_MEMORY, N_("Failed to allocate memory for %s"),
                    "TRACE");
            }
        }

        if ((map[i].size >> 3) > 0) {
            map[i].trace = (uint8_t*)calloc(map[i].size >> 3, sizeof(uint8_t));
            if (map[i].trace == NULL) {
                systemMessage(MSG_OUT_OF_MEMORY, N_("Failed to allocate memory for %s"),
                    "TRACE");
            }
        }
    }
    clearBreakRegList();
#endif
}
#endif

static void preLoadRomSetup(GBASys &gba)
{
  romSize = SIZE_ROM;
  /*if (rom != NULL) {
    CPUCleanUp();
  }*/

  systemSaveUpdateCounter = SYSTEM_SAVE_NOT_UPDATED;

  memset(workRAM, 0, sizeof(workRAM));
}

static void postLoadRomSetup(GBASys &gba)
{
  uint16_t *temp = (uint16_t *)(rom+((romSize+1)&~1));
  int i;
  for (i = (romSize+1)&~1; i < 0x2000000; i+=2) {
    WRITE16LE(temp, (i >> 1) & 0xFFFF);
    temp++;
  }

  memset(bios, 0, sizeof(bios));

  memset(internalRAM, 0, sizeof(internalRAM));

  memset(gba.mem.ioMem.b, 0, sizeof(gba.mem.ioMem));

  gba.lcd.reset();

  flashInit();
  eepromInit();

  CPUUpdateRenderBuffers(true);
}

int CPULoadRom(GBASys &gba, const char *szFile)
{
	preLoadRomSetup(gba);

  uint8_t* whereToLoad = coreOptions.cpuIsMultiBoot ? workRAM : rom;

#ifndef NO_DEBUGGER
  if (CPUIsELF(szFile)) {
    FILE* f = fopen(szFile, "rb");
    if (!f) {
      systemMessage(MSG_ERROR_OPENING_IMAGE, N_("Error opening image %s"),
                    szFile);
      return 0;
    }
    bool res = elfRead(szFile, romSize, f);
    if (!res || romSize == 0) {
      elfCleanUp();
      return 0;
    }
  } else
#endif //NO_DEBUGGER
  if (szFile != NULL) {
	  if (!utilLoad(szFile,
						  utilIsGBAImage,
						  whereToLoad,
						  romSize)) {
		return 0;
	  }
  }

  postLoadRomSetup(gba);

  return romSize;
}

#if 0
int CPULoadRomData(const char* data, int size)
{
    romSize = SIZE_ROM;
    if (rom != NULL) {
        CPUCleanUp();
    }

    systemSaveUpdateCounter = SYSTEM_SAVE_NOT_UPDATED;

    rom = (uint8_t*)malloc(SIZE_ROM);
    if (rom == NULL) {
        systemMessage(MSG_OUT_OF_MEMORY, N_("Failed to allocate memory for %s"),
            "ROM");
        return 0;
    }
    workRAM = (uint8_t*)calloc(1, SIZE_WRAM);
    if (workRAM == NULL) {
        systemMessage(MSG_OUT_OF_MEMORY, N_("Failed to allocate memory for %s"),
            "WRAM");
        return 0;
    }

    uint8_t* whereToLoad = coreOptions.cpuIsMultiBoot ? workRAM : rom;

    romSize = size % 2 == 0 ? size : size + 1;
    memcpy(whereToLoad, data, size);

    uint16_t* temp = (uint16_t*)(rom + ((romSize + 1) & ~1));
    int i;
    for (i = (romSize + 1) & ~1; i < SIZE_ROM; i += 2) {
        WRITE16LE(temp, (i >> 1) & 0xFFFF);
        temp++;
    }

    bios = (uint8_t*)calloc(1, SIZE_BIOS);
    if (bios == NULL) {
        systemMessage(MSG_OUT_OF_MEMORY, N_("Failed to allocate memory for %s"),
            "BIOS");
        CPUCleanUp();
        return 0;
    }
    internalRAM = (uint8_t*)calloc(1, SIZE_IRAM);
    if (internalRAM == NULL) {
        systemMessage(MSG_OUT_OF_MEMORY, N_("Failed to allocate memory for %s"),
            "IRAM");
        CPUCleanUp();
        return 0;
    }
    paletteRAM = (uint8_t*)calloc(1, SIZE_PRAM);
    if (paletteRAM == NULL) {
        systemMessage(MSG_OUT_OF_MEMORY, N_("Failed to allocate memory for %s"),
            "PRAM");
        CPUCleanUp();
        return 0;
    }
    vram = (uint8_t*)calloc(1, SIZE_VRAM);
    if (vram == NULL) {
        systemMessage(MSG_OUT_OF_MEMORY, N_("Failed to allocate memory for %s"),
            "VRAM");
        CPUCleanUp();
        return 0;
    }
    oam = (uint8_t*)calloc(1, SIZE_OAM);
    if (oam == NULL) {
        systemMessage(MSG_OUT_OF_MEMORY, N_("Failed to allocate memory for %s"),
            "OAM");
        CPUCleanUp();
        return 0;
    }

    pix = (uint8_t*)calloc(1, 4 * 240 * 160);
    if (pix == NULL) {
        systemMessage(MSG_OUT_OF_MEMORY, N_("Failed to allocate memory for %s"),
            "PIX");
        CPUCleanUp();
        return 0;
    }
    ioMem = (uint8_t*)calloc(1, SIZE_IOMEM);
    if (ioMem == NULL) {
        systemMessage(MSG_OUT_OF_MEMORY, N_("Failed to allocate memory for %s"),
            "IO");
        CPUCleanUp();
        return 0;
    }

    flashInit();
    eepromInit();

    CPUUpdateRenderBuffers(true);

    return romSize;
}
#endif

int CPULoadRomWithIO(GBASys &gba, IG::IO &io)
{
	preLoadRomSetup(gba);
	romSize = io.read(rom, romSize);
  postLoadRomSetup(gba);
  return romSize;
}

void doMirroring (GBASys &gba, bool b)
{
    int romSizeRounded = romSize;
    romSizeRounded--;
    romSizeRounded |= romSizeRounded >> 1;
    romSizeRounded |= romSizeRounded >> 2;
    romSizeRounded |= romSizeRounded >> 4;
    romSizeRounded |= romSizeRounded >> 8;
    romSizeRounded |= romSizeRounded >> 16;
    romSizeRounded++;
    uint32_t mirroredRomSize = (((romSizeRounded) >> 20) & 0x3F) << 20;
    uint32_t mirroredRomAddress = mirroredRomSize;
    if ((mirroredRomSize <= 0x800000) && (b)) {
        if (mirroredRomSize == 0)
            mirroredRomSize = 0x100000;
        while (mirroredRomAddress < 0x01000000) {
            memcpy((uint16_t*)(rom + mirroredRomAddress), (uint16_t*)(rom), mirroredRomSize);
            mirroredRomAddress += mirroredRomSize;
        }
    }
}

#if 0
const char* GetLoadDotCodeFile()
{
    return coreOptions.loadDotCodeFile;
}

const char* GetSaveDotCodeFile()
{
    return coreOptions.saveDotCodeFile;
}

void ResetLoadDotCodeFile()
{
    if (coreOptions.loadDotCodeFile) {
        free((char*)coreOptions.loadDotCodeFile);
    }

    coreOptions.loadDotCodeFile = strdup("");
}

void SetLoadDotCodeFile(const char* szFile)
{
    coreOptions.loadDotCodeFile = strdup(szFile);
}

void ResetSaveDotCodeFile()
{
    if (coreOptions.saveDotCodeFile) {
        free((char*)coreOptions.saveDotCodeFile);
    }

    coreOptions.saveDotCodeFile = strdup("");
}

void SetSaveDotCodeFile(const char* szFile)
{
    coreOptions.saveDotCodeFile = strdup(szFile);
}
#endif

void CPUUpdateRender(GBASys &gba)
{
  if (DISPCNT & 0x80)
  {
	  systemMessage(0, "Set forced blank");
	  gba.lcd.renderLine = ((DISPCNT & 7) == 0) ? blankLine : blankLineUpdateLastVCount;
  }
  else
  {
  	static const GBALCD::RenderLineFunc norm[8] =
  		{ mode0RenderLine, mode1RenderLine, mode2RenderLine, mode3RenderLine, mode4RenderLine, mode5RenderLine,
  		blankLine };
  	static const GBALCD::RenderLineFunc noWin[8] =
			{ mode0RenderLineNoWindow, mode1RenderLineNoWindow, mode2RenderLineNoWindow, mode3RenderLineNoWindow, mode4RenderLineNoWindow, mode5RenderLineNoWindow,
			blankLine };
  	static const GBALCD::RenderLineFunc all[8] =
			{ mode0RenderLineAll, mode1RenderLineAll, mode2RenderLineAll, mode3RenderLineAll, mode4RenderLineAll, mode5RenderLineAll,
			blankLine };
  	unsigned mode = DISPCNT & 7;
  	gba.lcd.renderLine = ((!fxOn && !windowOn && !(layerEnable & 0x8000)) || coreOptions.cpuDisableSfx) ? norm[mode] :
  			(fxOn && !windowOn && !(layerEnable & 0x8000)) ? noWin[mode] :
  			all[mode];
	  //systemMessage(0, "Set mode %s\n", dispModeName(renderLine));
  }
}

#define CPUUpdateRender() CPUUpdateRender(gba)

void ARM7TDMI::updateCPSR()
{
  uint32_t CPSR = reg[16].I & 0x40;
  if (nFlag())
    CPSR |= 0x80000000;
  if (zFlag())
    CPSR |= 0x40000000;
  if (C_FLAG)
    CPSR |= 0x20000000;
  if (V_FLAG)
    CPSR |= 0x10000000;
  if (!armState)
    CPSR |= 0x00000020;
  if (!armIrqEnable)
    CPSR |= 0x80;
  CPSR |= (armMode & 0x1F);
  reg[16].I = CPSR;
}

static void CPUUpdateCPSR(GBASys &gba)
{
	gba.cpu.updateCPSR();
}

void ARM7TDMI::updateFlags(const GBAMem::IoMem &ioMem, bool breakLoop)
{
  uint32_t CPSR = reg[16].I;

  bool N_FLAG = (CPSR & 0x80000000) ? true : false;
  bool Z_FLAG = (CPSR & 0x40000000) ? true : false;
  updateNZFlags(N_FLAG, Z_FLAG);
  C_FLAG = (CPSR & 0x20000000) ? true : false;
  V_FLAG = (CPSR & 0x10000000) ? true : false;
  armState = (CPSR & 0x20) ? false : true;
  armIrqEnable = (CPSR & 0x80) ? false : true;
  if (breakLoop) {
      if (armIrqEnable && (ioMem.IF & ioMem.IE) && (ioMem.IME & 1))
        cpuNextEvent = cpuTotalTicks;
  }
}

#ifdef WORDS_BIGENDIAN
static void CPUSwap(volatile uint32_t* a, volatile uint32_t* b)
{
    volatile uint32_t c = *b;
    *b = *a;
    *a = c;
}
#else
static void CPUSwap(uint32_t* a, uint32_t* b)
{
    uint32_t c = *b;
    *b = *a;
    *a = c;
}
#endif

void ARM7TDMI::switchMode(const GBAMem::IoMem &ioMem, int mode, bool saveState, bool breakLoop)
{
  //  if(armMode == mode)
  //    return;

  updateCPSR();

  switch (armMode) {
  case 0x10:
  case 0x1F:
    reg[R13_USR].I = reg[13].I;
    reg[R14_USR].I = reg[14].I;
    reg[17].I = reg[16].I;
    break;
  case 0x11:
    CPUSwap(&reg[R8_FIQ].I, &reg[8].I);
    CPUSwap(&reg[R9_FIQ].I, &reg[9].I);
    CPUSwap(&reg[R10_FIQ].I, &reg[10].I);
    CPUSwap(&reg[R11_FIQ].I, &reg[11].I);
    CPUSwap(&reg[R12_FIQ].I, &reg[12].I);
    reg[R13_FIQ].I = reg[13].I;
    reg[R14_FIQ].I = reg[14].I;
    reg[SPSR_FIQ].I = reg[17].I;
    break;
  case 0x12:
    reg[R13_IRQ].I  = reg[13].I;
    reg[R14_IRQ].I  = reg[14].I;
    reg[SPSR_IRQ].I =  reg[17].I;
    break;
  case 0x13:
    reg[R13_SVC].I  = reg[13].I;
    reg[R14_SVC].I  = reg[14].I;
    reg[SPSR_SVC].I =  reg[17].I;
    break;
  case 0x17:
    reg[R13_ABT].I  = reg[13].I;
    reg[R14_ABT].I  = reg[14].I;
    reg[SPSR_ABT].I =  reg[17].I;
    break;
  case 0x1b:
    reg[R13_UND].I  = reg[13].I;
    reg[R14_UND].I  = reg[14].I;
    reg[SPSR_UND].I =  reg[17].I;
    break;
  }

  uint32_t CPSR = reg[16].I;
  uint32_t SPSR = reg[17].I;

  switch (mode) {
  case 0x10:
  case 0x1F:
    reg[13].I = reg[R13_USR].I;
    reg[14].I = reg[R14_USR].I;
    reg[16].I = SPSR;
    break;
  case 0x11:
    CPUSwap(&reg[8].I, &reg[R8_FIQ].I);
    CPUSwap(&reg[9].I, &reg[R9_FIQ].I);
    CPUSwap(&reg[10].I, &reg[R10_FIQ].I);
    CPUSwap(&reg[11].I, &reg[R11_FIQ].I);
    CPUSwap(&reg[12].I, &reg[R12_FIQ].I);
    reg[13].I = reg[R13_FIQ].I;
    reg[14].I = reg[R14_FIQ].I;
    if (saveState)
      reg[17].I = CPSR;
    else
      reg[17].I = reg[SPSR_FIQ].I;
    break;
  case 0x12:
    reg[13].I = reg[R13_IRQ].I;
    reg[14].I = reg[R14_IRQ].I;
    reg[16].I = SPSR;
    if (saveState)
      reg[17].I = CPSR;
    else
      reg[17].I = reg[SPSR_IRQ].I;
    break;
  case 0x13:
    reg[13].I = reg[R13_SVC].I;
    reg[14].I = reg[R14_SVC].I;
    reg[16].I = SPSR;
    if (saveState)
      reg[17].I = CPSR;
    else
      reg[17].I = reg[SPSR_SVC].I;
    break;
  case 0x17:
    reg[13].I = reg[R13_ABT].I;
    reg[14].I = reg[R14_ABT].I;
    reg[16].I = SPSR;
    if (saveState)
      reg[17].I = CPSR;
    else
      reg[17].I = reg[SPSR_ABT].I;
    break;
  case 0x1b:
    reg[13].I = reg[R13_UND].I;
    reg[14].I = reg[R14_UND].I;
    reg[16].I = SPSR;
    if (saveState)
      reg[17].I = CPSR;
    else
      reg[17].I = reg[SPSR_UND].I;
    break;
  default:
    systemMessage(MSG_UNSUPPORTED_ARM_MODE, N_("Unsupported ARM mode %02x"), mode);
    break;
  }
  armMode = mode;
  updateFlags(ioMem, breakLoop);
  updateCPSR();
}

void ARM7TDMI::switchMode(const GBAMem::IoMem &ioMem, int mode, bool saveState)
{
  switchMode(ioMem, mode, saveState, true);
}

void ARM7TDMI::undefinedException(const GBAMem::IoMem &ioMem)
{
    uint32_t PC = reg[15].I;
    bool savedArmState = armState;
    switchMode(ioMem, 0x1b, true, false);
    reg[14].I = PC - (savedArmState ? 4 : 2);
    reg[15].I = 0x04;
    armState = true;
    armIrqEnable = false;
    armNextPC = 0x04;
    ARM_PREFETCH();
    reg[15].I += 4;
}

void ARM7TDMI::undefinedException()
{
	undefinedException(gba->mem.ioMem);
}

void ARM7TDMI::softwareInterrupt(const GBAMem::IoMem &ioMem)
{
  uint32_t PC = reg[15].I;
  bool savedArmState = armState;
  switchMode(ioMem, 0x13, true, false);
  reg[14].I = PC - (savedArmState ? 4 : 2);
  reg[15].I = 0x08;
  armState = true;
  armIrqEnable = false;
  armNextPC = 0x08;
  ARM_PREFETCH();
  reg[15].I += 4;
}

void ARM7TDMI::softwareInterrupt()
{
	softwareInterrupt(gba->mem.ioMem);
}

#define cpuNextEvent cpu.cpuNextEvent
#define cpuTotalTicks cpu.cpuTotalTicks
#define IF gba.mem.ioMem.IF
#define armState cpu.armState
#define armMode cpu.armMode
#define armNextPC cpu.armNextPC

void CPUSoftwareInterrupt(ARM7TDMI &cpu, int comment)
{
	auto &gba = *cpu.gba;
	auto &reg = cpu.reg;
	auto &holdState = cpu.holdState;
  static bool disableMessage = false;
#ifdef BKPT_SUPPORT
  if (comment == 0xff) {
    dbgOutput(NULL, reg[0].I);
    return;
  }
#endif
#ifdef PROFILING
  if (comment == 0xfe) {
    profStartup(reg[0].I, reg[1].I);
    return;
  }
  if (comment == 0xfd) {
    profControl(reg[0].I);
    return;
  }
  if (comment == 0xfc) {
    profCleanup();
    return;
  }
  if (comment == 0xfb) {
    profCount();
    return;
  }
#endif
  if (comment == 0xfa) {
    agbPrintFlush();
    return;
  }
#ifdef SDL
  if (comment == 0xf9) {
    emulating = 0;
    cpuNextEvent = cpuTotalTicks;
    cpuBreakLoop = true;
    return;
  }
#endif
  if (coreOptions.useBios) {
#ifdef GBA_LOGGING
    if (systemVerbose & VERBOSE_SWI) {
      log("SWI: %08x at %08x (0x%08x,0x%08x,0x%08x,VCOUNT = %2d)\n", comment,
          armState ? armNextPC - 4 : armNextPC - 2,
          reg[0].I,
          reg[1].I,
          reg[2].I,
          VCOUNT);
    }
#endif
    if ((comment & 0xF8) != 0xE0) {
    	  cpu.softwareInterrupt();
        return;
    } else {
        if (CheckEReaderRegion())
            BIOS_EReader_ScanCard(comment);
        else
            cpu.softwareInterrupt();
        return;
    }
  }
  // This would be correct, but it causes problems if uncommented
  //  else {
  //    biosProtected = 0xe3a02004;
  //  }

  switch (comment) {
  case 0x00:
    BIOS_SoftReset(cpu);
    cpu.ARM_PREFETCH();
    break;
  case 0x01:
    BIOS_RegisterRamReset(cpu);
    break;
  case 0x02:
#ifdef GBA_LOGGING
    if (systemVerbose & VERBOSE_SWI) {
      /*log("Halt: (VCOUNT = %2d)\n",
          VCOUNT);*/
    }
#endif
    holdState = true;
    holdType = -1;
    cpuNextEvent = cpuTotalTicks;
    break;
  case 0x03:
#ifdef GBA_LOGGING
    if (systemVerbose & VERBOSE_SWI) {
      /*log("Stop: (VCOUNT = %2d)\n",
          VCOUNT);*/
    }
#endif
    holdState = true;
    holdType = -1;
    stopState = true;
    cpuNextEvent = cpuTotalTicks;
    break;
  case 0x04:
#ifdef GBA_LOGGING
    if (systemVerbose & VERBOSE_SWI) {
      log("IntrWait: 0x%08x,0x%08x (VCOUNT = %2d)\n",
          reg[0].I,
          reg[1].I,
          VCOUNT);
    }
#endif
    cpu.softwareInterrupt();
    break;
  case 0x05:
#ifdef GBA_LOGGING
    if (systemVerbose & VERBOSE_SWI) {
      log("VBlankIntrWait: (VCOUNT = %2d)\n",
          VCOUNT);
    }
#endif
    cpu.softwareInterrupt();
    break;
  case 0x06:
  	cpu.softwareInterrupt();
    break;
  case 0x07:
  	cpu.softwareInterrupt();
    break;
  case 0x08:
    BIOS_Sqrt(cpu);
    break;
  case 0x09:
    BIOS_ArcTan(cpu);
    break;
  case 0x0A:
    BIOS_ArcTan2(cpu);
    break;
  case 0x0B: {
    int len = (reg[2].I & 0x1FFFFF) >> 1;
    if (!(((reg[0].I & 0xe000000) == 0) || ((reg[0].I + len) & 0xe000000) == 0)) {
        if ((reg[2].I >> 24) & 1) {
            if ((reg[2].I >> 26) & 1)
                SWITicks = (7 + memoryWait32[(reg[1].I >> 24) & 0xF]) * (len >> 1);
            else
                SWITicks = (8 + memoryWait[(reg[1].I >> 24) & 0xF]) * (len);
        } else {
            if ((reg[2].I >> 26) & 1)
                SWITicks = (10 + memoryWait32[(reg[0].I >> 24) & 0xF] + memoryWait32[(reg[1].I >> 24) & 0xF]) * (len >> 1);
            else
                SWITicks = (11 + memoryWait[(reg[0].I >> 24) & 0xF] + memoryWait[(reg[1].I >> 24) & 0xF]) * len;
        }
    }
  }
    BIOS_CpuSet(cpu);
    break;
  case 0x0C: {
    int len = (reg[2].I & 0x1FFFFF) >> 5;
    if (!(((reg[0].I & 0xe000000) == 0) || ((reg[0].I + len) & 0xe000000) == 0)) {
        if ((reg[2].I >> 24) & 1)
            SWITicks = (6 + memoryWait32[(reg[1].I >> 24) & 0xF] + 7 * (memoryWaitSeq32[(reg[1].I >> 24) & 0xF] + 1)) * len;
        else
            SWITicks = (9 + memoryWait32[(reg[0].I >> 24) & 0xF] + memoryWait32[(reg[1].I >> 24) & 0xF] + 7 * (memoryWaitSeq32[(reg[0].I >> 24) & 0xF] + memoryWaitSeq32[(reg[1].I >> 24) & 0xF] + 2)) * len;
    }
  }
    BIOS_CpuFastSet(cpu);
    break;
  case 0x0D:
    BIOS_GetBiosChecksum(cpu);
    break;
  case 0x0E:
    BIOS_BgAffineSet(cpu);
    break;
  case 0x0F:
    BIOS_ObjAffineSet(cpu);
    break;
  case 0x10: {
    int len = CPUReadHalfWord(reg[2].I);
    if (!(((reg[0].I & 0xe000000) == 0) || ((reg[0].I + len) & 0xe000000) == 0))
        SWITicks = (32 + memoryWait[(reg[0].I >> 24) & 0xF]) * len;
  }
    BIOS_BitUnPack(cpu);
    break;
  case 0x11: {
    uint32_t len = CPUReadMemory(reg[0].I) >> 8;
    if (!(((reg[0].I & 0xe000000) == 0) || ((reg[0].I + (len & 0x1fffff)) & 0xe000000) == 0))
        SWITicks = (9 + memoryWait[(reg[1].I >> 24) & 0xF]) * len;
  }
    BIOS_LZ77UnCompWram(cpu);
    break;
  case 0x12: {
    uint32_t len = CPUReadMemory(reg[0].I) >> 8;
    if (!(((reg[0].I & 0xe000000) == 0) || ((reg[0].I + (len & 0x1fffff)) & 0xe000000) == 0))
        SWITicks = (19 + memoryWait[(reg[1].I >> 24) & 0xF]) * len;
  }
    BIOS_LZ77UnCompVram(cpu);
    break;
  case 0x13: {
    uint32_t len = CPUReadMemory(reg[0].I) >> 8;
    if (!(((reg[0].I & 0xe000000) == 0) || ((reg[0].I + (len & 0x1fffff)) & 0xe000000) == 0))
        SWITicks = (29 + (memoryWait[(reg[0].I >> 24) & 0xF] << 1)) * len;
  }
    BIOS_HuffUnComp(cpu);
    break;
  case 0x14: {
    uint32_t len = CPUReadMemory(reg[0].I) >> 8;
    if (!(((reg[0].I & 0xe000000) == 0) || ((reg[0].I + (len & 0x1fffff)) & 0xe000000) == 0))
        SWITicks = (11 + memoryWait[(reg[0].I >> 24) & 0xF] + memoryWait[(reg[1].I >> 24) & 0xF]) * len;
  }
    BIOS_RLUnCompWram(cpu);
    break;
  case 0x15: {
    uint32_t len = CPUReadMemory(reg[0].I) >> 9;
    if (!(((reg[0].I & 0xe000000) == 0) || ((reg[0].I + (len & 0x1fffff)) & 0xe000000) == 0))
        SWITicks = (34 + (memoryWait[(reg[0].I >> 24) & 0xF] << 1) + memoryWait[(reg[1].I >> 24) & 0xF]) * len;
  }
    BIOS_RLUnCompVram(cpu);
    break;
  case 0x16: {
    uint32_t len = CPUReadMemory(reg[0].I) >> 8;
    if (!(((reg[0].I & 0xe000000) == 0) || ((reg[0].I + (len & 0x1fffff)) & 0xe000000) == 0))
        SWITicks = (13 + memoryWait[(reg[0].I >> 24) & 0xF] + memoryWait[(reg[1].I >> 24) & 0xF]) * len;
  }
    BIOS_Diff8bitUnFilterWram(cpu);
    break;
  case 0x17: {
    uint32_t len = CPUReadMemory(reg[0].I) >> 9;
    if (!(((reg[0].I & 0xe000000) == 0) || ((reg[0].I + (len & 0x1fffff)) & 0xe000000) == 0))
        SWITicks = (39 + (memoryWait[(reg[0].I >> 24) & 0xF] << 1) + memoryWait[(reg[1].I >> 24) & 0xF]) * len;
  }
    BIOS_Diff8bitUnFilterVram(cpu);
    break;
  case 0x18: {
    uint32_t len = CPUReadMemory(reg[0].I) >> 9;
    if (!(((reg[0].I & 0xe000000) == 0) || ((reg[0].I + (len & 0x1fffff)) & 0xe000000) == 0))
        SWITicks = (13 + memoryWait[(reg[0].I >> 24) & 0xF] + memoryWait[(reg[1].I >> 24) & 0xF]) * len;
  }
    BIOS_Diff16bitUnFilter(cpu);
    break;
  case 0x19:
#ifdef GBA_LOGGING
    if (systemVerbose & VERBOSE_SWI) {
      log("SoundBiasSet: 0x%08x (VCOUNT = %2d)\n",
          reg[0].I,
          VCOUNT);
    }
#endif
    if (reg[0].I)
      soundPause();
    else
      soundResume();
    break;
    case 0x1A:
        BIOS_SndDriverInit(cpu);
        SWITicks = 252000;
    break;
    case 0x1B:
        BIOS_SndDriverMode(cpu);
        SWITicks = 280000;
    break;
    case 0x1C:
        BIOS_SndDriverMain(cpu);
        SWITicks = 11050; //avg
    break;
    case 0x1D:
         BIOS_SndDriverVSync(cpu);
         SWITicks = 44;
    break;
    case 0x1E:
         BIOS_SndChannelClear(cpu);
    break;
    case 0x28:
         BIOS_SndDriverVSyncOff(cpu);
    break;
    case 0x1F:
         BIOS_MidiKey2Freq(cpu);
    break;
    case 0xE0:
    case 0xE1:
    case 0xE2:
    case 0xE3:
    case 0xE4:
    case 0xE5:
    case 0xE6:
    case 0xE7:
        if (CheckEReaderRegion())
            BIOS_EReader_ScanCard(comment);
    break;
  case 0x2A:
    BIOS_SndDriverJmpTableCopy(cpu);
    // let it go, because we don't really emulate this function
    /* fallthrough */
  default:
#ifdef GBA_LOGGING
    if (systemVerbose & VERBOSE_SWI) {
      log("SWI: %08x at %08x (0x%08x,0x%08x,0x%08x,VCOUNT = %2d)\n", comment,
          armState ? armNextPC - 4 : armNextPC - 2,
          reg[0].I,
          reg[1].I,
          reg[2].I,
          VCOUNT);
    }
#endif

    if (!disableMessage) {
      systemMessage(MSG_UNSUPPORTED_BIOS_FUNCTION,
                    N_("Unsupported BIOS function %02x called from %08x. A BIOS file is needed in order to get correct behaviour."),
                    comment,
										armMode ? armNextPC - 4 : armNextPC - 2);
      disableMessage = true;
    }
    break;
  }
}

void CPUCompareVCOUNT(ARM7TDMI &cpu)
{
	auto &gba = *cpu.gba;
  if (VCOUNT == (DISPSTAT >> 8)) {
  	DISPSTAT |= 4;
    UPDATE_REG(0x04, DISPSTAT);

    if (DISPSTAT & 0x20) {
    	IF |= 4;
      UPDATE_REG(0x202, IF);
    }
  } else {
  	DISPSTAT &= 0xFFFB;
    UPDATE_REG(0x4, DISPSTAT);
  }
  if (layerEnableDelay > 0) {
  	layerEnableDelay--;
      if (layerEnableDelay == 1)
      	layerEnable = coreOptions.layerSettings & DISPCNT;
  }

}

#define CPUCompareVCOUNT() CPUCompareVCOUNT(cpu)
#define CPUWriteMemory(a, d) CPUWriteMemory(cpu, a, d)
#define CPUWriteHalfWord(a, d) CPUWriteHalfWord(cpu, a, d)
#define cpuDmaLast gba.dma.cpuDmaLast
#define cpuDmaRunning gba.dma.cpuDmaRunning
#define cpuDmaPC gba.dma.cpuDmaPC
#define cpuDmaTicksToUpdate gba.dma.cpuDmaTicksToUpdate

void doDMA(GBASys &gba, ARM7TDMI &cpu, uint32_t &s, uint32_t &d, uint32_t si, uint32_t di, uint32_t c, int transfer32)
{
	auto &reg = cpu.reg;
  int sm = s >> 24;
  int dm = d >> 24;
  int sw = 0;
  int dw = 0;
  int sc = c;

  cpuDmaRunning = true;
  cpuDmaPC = reg[15].I;
  cpuDmaCount = c;
  // This is done to get the correct waitstates.
  if (sm > 15)
      sm = 15;
  if (dm > 15)
      dm = 15;

  //if ((sm>=0x05) && (sm<=0x07) || (dm>=0x05) && (dm <=0x07))
  //    blank = (((DISPSTAT | ((DISPSTAT>>1)&1))==1) ?  true : false);

  if (transfer32) {
    s &= 0xFFFFFFFC;
    if (s < 0x02000000 && (reg[15].I >> 24)) {
      while (c != 0) {
        CPUWriteMemory(d, 0);
        d += di;
        c--;
      }
    } else {
      while (c != 0) {
      	cpuDmaLast = CPUReadMemory(s);
        CPUWriteMemory(d, cpuDmaLast);
        d += di;
        s += si;
        c--;
      }
    }
  } else {
    s &= 0xFFFFFFFE;
    si = (int)si >> 1;
    di = (int)di >> 1;
    if (s < 0x02000000 && (reg[15].I >> 24)) {
      while (c != 0) {
        CPUWriteHalfWord(d, 0);
        d += di;
        c--;
      }
    } else {
      while (c != 0) {
      	cpuDmaLast = CPUReadHalfWord(s);
        CPUWriteHalfWord(d, cpuDmaLast);
        cpuDmaLast |= (cpuDmaLast << 16);
        d += di;
        s += si;
        c--;
      }
    }
  }

  cpuDmaCount = 0;

  int totalTicks = 0;

  if (transfer32) {
      sw = 1 + memoryWaitSeq32[sm & 15];
      dw = 1 + memoryWaitSeq32[dm & 15];
      totalTicks = (sw + dw) * (sc - 1) + 6 + memoryWait32[sm & 15] + memoryWaitSeq32[dm & 15];
  } else {
     sw = 1 + memoryWaitSeq[sm & 15];
     dw = 1 + memoryWaitSeq[dm & 15];
      totalTicks = (sw + dw) * (sc - 1) + 6 + memoryWait[sm & 15] + memoryWaitSeq[dm & 15];
  }

  cpuDmaTicksToUpdate += totalTicks;
  cpuDmaRunning = false;
}

#define doDMA(s, d, si, di, c, transfer32) doDMA(gba, cpu, s, d, si, di, c, transfer32)

void CPUCheckDMA(GBASys &gba, ARM7TDMI &cpu, int reason, int dmamask)
{
	static const uint32_t addrControlMap[4] = { 4, (uint32_t)-4, 0, 4 };
  // DMA 0
  if ((DM0CNT_H & 0x8000) && (dmamask & 1)) {
    if (((DM0CNT_H >> 12) & 3) == reason) {
      uint32_t sourceIncrement = addrControlMap[(DM0CNT_H >> 7) & 3];
      uint32_t destIncrement = addrControlMap[(DM0CNT_H >> 5) & 3];
#ifdef GBA_LOGGING
      if (systemVerbose & VERBOSE_DMA0) {
        int count = (DM0CNT_L ? DM0CNT_L : 0x4000) << 1;
        if (DM0CNT_H & 0x0400)
          count <<= 1;
        log("DMA0: s=%08x d=%08x c=%04x count=%08x\n", dma0Source, dma0Dest,
            DM0CNT_H,
            count);
      }
#endif
      doDMA(dma0Source, dma0Dest, sourceIncrement, destIncrement,
      		DM0CNT_L ? DM0CNT_L : 0x4000,
      				DM0CNT_H & 0x0400);

      if (DM0CNT_H & 0x4000) {
        IF |= 0x0100;
        UPDATE_REG(0x202, IF);
        cpuNextEvent = cpuTotalTicks;
      }

      if (((DM0CNT_H >> 5) & 3) == 3) {
        dma0Dest = DM0DAD_L | (DM0DAD_H << 16);
      }

      if (!(DM0CNT_H & 0x0200) || (reason == 0)) {
      	DM0CNT_H &= 0x7FFF;
        UPDATE_REG(0xBA, DM0CNT_H);
      }
    }
  }

  // DMA 1
  if ((DM1CNT_H & 0x8000) && (dmamask & 2)) {
    if (((DM1CNT_H >> 12) & 3) == reason) {
      uint32_t sourceIncrement = addrControlMap[(DM1CNT_H >> 7) & 3];
      uint32_t destIncrement = addrControlMap[(DM1CNT_H >> 5) & 3];
      if (reason == 3) {
#ifdef GBA_LOGGING
        if (systemVerbose & VERBOSE_DMA1) {
          log("DMA1: s=%08x d=%08x c=%04x count=%08x\n", dma1Source, dma1Dest,
              DM1CNT_H,
              16);
        }
#endif
        doDMA(dma1Source, dma1Dest, sourceIncrement, 0, 4,
              0x0400);
      } else {
#ifdef GBA_LOGGING
        if (systemVerbose & VERBOSE_DMA1) {
          int count = (DM1CNT_L ? DM1CNT_L : 0x4000) << 1;
          if (DM1CNT_H & 0x0400)
            count <<= 1;
          log("DMA1: s=%08x d=%08x c=%04x count=%08x\n", dma1Source, dma1Dest,
              DM1CNT_H,
              count);
        }
#endif
        doDMA(dma1Source, dma1Dest, sourceIncrement, destIncrement,
        		DM1CNT_L ? DM1CNT_L : 0x4000,
        				DM1CNT_H & 0x0400);
      }

      if (DM1CNT_H & 0x4000) {
        IF |= 0x0200;
        UPDATE_REG(0x202, IF);
        cpuNextEvent = cpuTotalTicks;
      }

      if (((DM1CNT_H >> 5) & 3) == 3) {
        dma1Dest = DM1DAD_L | (DM1DAD_H << 16);
      }

      if (!(DM1CNT_H & 0x0200) || (reason == 0)) {
      	DM1CNT_H &= 0x7FFF;
        UPDATE_REG(0xC6, DM1CNT_H);
      }
    }
  }

  // DMA 2
  if ((DM2CNT_H & 0x8000) && (dmamask & 4)) {
    if (((DM2CNT_H >> 12) & 3) == reason) {
      uint32_t sourceIncrement = addrControlMap[(DM2CNT_H >> 7) & 3];
      uint32_t destIncrement = addrControlMap[(DM2CNT_H >> 5) & 3];
      if (reason == 3) {
#ifdef GBA_LOGGING
        if (systemVerbose & VERBOSE_DMA2) {
          int count = (4) << 2;
          log("DMA2: s=%08x d=%08x c=%04x count=%08x\n", dma2Source, dma2Dest,
              DM2CNT_H,
              count);
        }
#endif
        doDMA(dma2Source, dma2Dest, sourceIncrement, 0, 4,
              0x0400);
      } else {
#ifdef GBA_LOGGING
        if (systemVerbose & VERBOSE_DMA2) {
          int count = (DM2CNT_L ? DM2CNT_L : 0x4000) << 1;
          if (DM2CNT_H & 0x0400)
            count <<= 1;
          log("DMA2: s=%08x d=%08x c=%04x count=%08x\n", dma2Source, dma2Dest,
              DM2CNT_H,
              count);
        }
#endif
        doDMA(dma2Source, dma2Dest, sourceIncrement, destIncrement,
        		DM2CNT_L ? DM2CNT_L : 0x4000,
        				DM2CNT_H & 0x0400);
      }

      if (DM2CNT_H & 0x4000) {
        IF |= 0x0400;
        UPDATE_REG(0x202, IF);
        cpuNextEvent = cpuTotalTicks;
      }

      if (((DM2CNT_H >> 5) & 3) == 3) {
        dma2Dest = DM2DAD_L | (DM2DAD_H << 16);
      }

      if (!(DM2CNT_H & 0x0200) || (reason == 0)) {
      	DM2CNT_H &= 0x7FFF;
        UPDATE_REG(0xD2, DM2CNT_H);
      }
    }
  }

  // DMA 3
  if ((DM3CNT_H & 0x8000) && (dmamask & 8)) {
    if (((DM3CNT_H >> 12) & 3) == reason) {
      uint32_t sourceIncrement = addrControlMap[(DM3CNT_H >> 7) & 3];
      uint32_t destIncrement = addrControlMap[(DM3CNT_H >> 5) & 3];
#ifdef GBA_LOGGING
      if (systemVerbose & VERBOSE_DMA3) {
        int count = (DM3CNT_L ? DM3CNT_L : 0x10000) << 1;
        if (DM3CNT_H & 0x0400)
          count <<= 1;
        log("DMA3: s=%08x d=%08x c=%04x count=%08x\n", dma3Source, dma3Dest,
            DM3CNT_H,
            count);
      }
#endif
      doDMA(dma3Source, dma3Dest, sourceIncrement, destIncrement,
      		DM3CNT_L ? DM3CNT_L : 0x10000,
      				DM3CNT_H & 0x0400);
      if (DM3CNT_H & 0x4000) {
        IF |= 0x0800;
        UPDATE_REG(0x202, IF);
        cpuNextEvent = cpuTotalTicks;
      }

      if (((DM3CNT_H >> 5) & 3) == 3) {
        dma3Dest = DM3DAD_L | (DM3DAD_H << 16);
      }

      if (!(DM3CNT_H & 0x0200) || (reason == 0)) {
      	DM3CNT_H &= 0x7FFF;
        UPDATE_REG(0xDE, DM3CNT_H);
      }
    }
  }
}

#define CPUCheckDMA(reason, dmamask) CPUCheckDMA(gba, cpu, reason, dmamask)
#define timerOnOffDelay gba.timers.timerOnOffDelay
#define soundTimerOverflow(v) soundTimerOverflow(gba, cpu, v)
#define soundEvent8(addr, data) soundEvent8(gba, addr, data)
#define soundEvent16(addr, data) soundEvent16(gba, addr, data)

void CPUUpdateRegister(ARM7TDMI &cpu, uint32_t address, uint16_t value)
{
	auto &armIrqEnable = cpu.armIrqEnable;
	auto &IE = cpu.gba->mem.ioMem.IE;
	auto &IME = cpu.gba->mem.ioMem.IME;
	auto &busPrefetchCount = cpu.busPrefetchCount;
	auto &busPrefetch = cpu.busPrefetch;
	auto &busPrefetchEnable = cpu.busPrefetchEnable;
	auto &ioMem = cpu.gba->mem.ioMem;
	auto &gba = *cpu.gba;

  switch (address) {
  case 0x00: { // we need to place the following code in { } because we declare & initialize variables in a case statement
      if ((value & 7) > 5) {
        // display modes above 0-5 are prohibited
      	DISPCNT = (value & 7);
      }
      bool change = (0 != ((DISPCNT ^ value) & 0x80));
      bool changeBG = (0 != ((DISPCNT ^ value) & 0x0F00));
      uint16_t changeBGon = ((~DISPCNT) & value) & 0x0F00; // these layers are being activated

      DISPCNT = (value & 0xFFF7); // bit 3 can only be accessed by the BIOS to enable GBC mode
      UPDATE_REG(0x00, DISPCNT);

      if (changeBGon) {
      	layerEnableDelay = 4;
      	layerEnable = coreOptions.layerSettings & value & (~changeBGon);
      } else {
      	layerEnable = coreOptions.layerSettings & value;
        // CPUUpdateTicks();
      }

      windowOn = (layerEnable & 0x6000) ? true : false;
      if (change && !((value & 0x80))) {
        if (!(DISPSTAT & 1)) {
        	//lcdTicks = 1008;
          //      VCOUNT = 0;
          //      UPDATE_REG(0x06, VCOUNT);
          DISPSTAT &= 0xFFFC;
          UPDATE_REG(0x04, DISPSTAT);
          CPUCompareVCOUNT();
        }
        //        (*renderLine)();
      }
      CPUUpdateRender();
      // we only care about changes in BG0-BG3
      if (changeBG) {
        CPUUpdateRenderBuffers(false);
      }
      break;
    }
  case 0x04:
  	DISPSTAT = (value & 0xFF38) | (DISPSTAT & 7);
    UPDATE_REG(0x04, DISPSTAT);
    break;
  case 0x06:
    // not writable
    break;
  case 0x08:
  	BG0CNT = (value & 0xDFCF);
    UPDATE_REG(0x08, BG0CNT);
    break;
  case 0x0A:
  	BG1CNT = (value & 0xDFCF);
    UPDATE_REG(0x0A, BG1CNT);
    break;
  case 0x0C:
  	BG2CNT = (value & 0xFFCF);
    UPDATE_REG(0x0C, BG2CNT);
    break;
  case 0x0E:
  	BG3CNT = (value & 0xFFCF);
    UPDATE_REG(0x0E, BG3CNT);
    break;
  case 0x10:
  	BG0HOFS = value & 511;
    UPDATE_REG(0x10, BG0HOFS);
    break;
  case 0x12:
  	BG0VOFS = value & 511;
    UPDATE_REG(0x12, BG0VOFS);
    break;
  case 0x14:
  	BG1HOFS = value & 511;
    UPDATE_REG(0x14, BG1HOFS);
    break;
  case 0x16:
  	BG1VOFS = value & 511;
    UPDATE_REG(0x16, BG1VOFS);
    break;
  case 0x18:
  	BG2HOFS = value & 511;
    UPDATE_REG(0x18, BG2HOFS);
    break;
  case 0x1A:
  	BG2VOFS = value & 511;
    UPDATE_REG(0x1A, BG2VOFS);
    break;
  case 0x1C:
  	BG3HOFS = value & 511;
    UPDATE_REG(0x1C, BG3HOFS);
    break;
  case 0x1E:
  	BG3VOFS = value & 511;
    UPDATE_REG(0x1E, BG3VOFS);
    break;
  case 0x20:
  	BG2PA = value;
    UPDATE_REG(0x20, BG2PA);
    break;
  case 0x22:
  	BG2PB = value;
    UPDATE_REG(0x22, BG2PB);
    break;
  case 0x24:
  	BG2PC = value;
    UPDATE_REG(0x24, BG2PC);
    break;
  case 0x26:
  	BG2PD = value;
    UPDATE_REG(0x26, BG2PD);
    break;
  case 0x28:
  	BG2X_L = value;
    UPDATE_REG(0x28, BG2X_L);
  	gfxBG2Changed |= 1;
    break;
  case 0x2A:
  	BG2X_H = (value & 0xFFF);
    UPDATE_REG(0x2A, BG2X_H);
  	gfxBG2Changed |= 1;
    break;
  case 0x2C:
  	BG2Y_L = value;
    UPDATE_REG(0x2C, BG2Y_L);
  	gfxBG2Changed |= 2;
    break;
  case 0x2E:
  	BG2Y_H = value & 0xFFF;
    UPDATE_REG(0x2E, BG2Y_H);
  	gfxBG2Changed |= 2;
    break;
  case 0x30:
  	BG3PA = value;
    UPDATE_REG(0x30, BG3PA);
    break;
  case 0x32:
  	BG3PB = value;
    UPDATE_REG(0x32, BG3PB);
    break;
  case 0x34:
  	BG3PC = value;
    UPDATE_REG(0x34, BG3PC);
    break;
  case 0x36:
  	BG3PD = value;
    UPDATE_REG(0x36, BG3PD);
    break;
  case 0x38:
  	BG3X_L = value;
    UPDATE_REG(0x38, BG3X_L);
  	gfxBG3Changed |= 1;
    break;
  case 0x3A:
  	BG3X_H = value & 0xFFF;
    UPDATE_REG(0x3A, BG3X_H);
  	gfxBG3Changed |= 1;
    break;
  case 0x3C:
  	BG3Y_L = value;
    UPDATE_REG(0x3C, BG3Y_L);
  	gfxBG3Changed |= 2;
    break;
  case 0x3E:
  	BG3Y_H = value & 0xFFF;
    UPDATE_REG(0x3E, BG3Y_H);
  	gfxBG3Changed |= 2;
    break;
  case 0x40:
  	WIN0H = value;
    UPDATE_REG(0x40, WIN0H);
    CPUUpdateWindow0();
    break;
  case 0x42:
  	WIN1H = value;
    UPDATE_REG(0x42, WIN1H);
    CPUUpdateWindow1();
    break;
  case 0x44:
  	WIN0V = value;
    UPDATE_REG(0x44, WIN0V);
    break;
  case 0x46:
  	WIN1V = value;
    UPDATE_REG(0x46, WIN1V);
    break;
  case 0x48:
  	WININ = value & 0x3F3F;
    UPDATE_REG(0x48, WININ);
    break;
  case 0x4A:
  	WINOUT = value & 0x3F3F;
    UPDATE_REG(0x4A, WINOUT);
    break;
  case 0x4C:
  	MOSAIC = value;
    UPDATE_REG(0x4C, MOSAIC);
    break;
  case 0x50:
  	BLDMOD = value & 0x3FFF;
    UPDATE_REG(0x50, BLDMOD);
  	fxOn = ((BLDMOD >> 6) & 3) != 0;
    CPUUpdateRender();
    break;
  case 0x52:
  	COLEV = value & 0x1F1F;
    UPDATE_REG(0x52, COLEV);
    break;
  case 0x54:
  	COLY = value & 0x1F;
    UPDATE_REG(0x54, COLY);
    break;
  case 0x60:
  case 0x62:
  case 0x64:
  case 0x68:
  case 0x6c:
  case 0x70:
  case 0x72:
  case 0x74:
  case 0x78:
  case 0x7c:
  case 0x80:
  case 0x84:
  	soundEvent8(address & 0xFF, (uint8_t)(value & 0xFF));
  	soundEvent8((address & 0xFF) + 1, (uint8_t)(value >> 8));
    break;
  case 0x82:
  case 0x88:
  case 0xa0:
  case 0xa2:
  case 0xa4:
  case 0xa6:
  case 0x90:
  case 0x92:
  case 0x94:
  case 0x96:
  case 0x98:
  case 0x9a:
  case 0x9c:
  case 0x9e:
  	soundEvent16(address & 0xFF, value);
    break;
  case 0xB0:
  	DM0SAD_L = value;
    UPDATE_REG(0xB0, DM0SAD_L);
    break;
  case 0xB2:
  	DM0SAD_H = value & 0x07FF;
    UPDATE_REG(0xB2, DM0SAD_H);
    break;
  case 0xB4:
  	DM0DAD_L = value;
    UPDATE_REG(0xB4, DM0DAD_L);
    break;
  case 0xB6:
  	DM0DAD_H = value & 0x07FF;
    UPDATE_REG(0xB6, DM0DAD_H);
    break;
  case 0xB8:
  	DM0CNT_L = value & 0x3FFF;
    UPDATE_REG(0xB8, 0);
    break;
  case 0xBA: {
      bool start = ((DM0CNT_H ^ value) & 0x8000) ? true : false;
      value &= 0xF7E0;

      DM0CNT_H = value;
      UPDATE_REG(0xBA, DM0CNT_H);

      if (start && (value & 0x8000)) {
        dma0Source = DM0SAD_L | (DM0SAD_H << 16);
        dma0Dest = DM0DAD_L | (DM0DAD_H << 16);
        CPUCheckDMA(0, 1);
      }
    } break;
  case 0xBC:
  	DM1SAD_L = value;
    UPDATE_REG(0xBC, DM1SAD_L);
    break;
  case 0xBE:
  	DM1SAD_H = value & 0x0FFF;
    UPDATE_REG(0xBE, DM1SAD_H);
    break;
  case 0xC0:
  	DM1DAD_L = value;
    UPDATE_REG(0xC0, DM1DAD_L);
    break;
  case 0xC2:
  	DM1DAD_H = value & 0x07FF;
    UPDATE_REG(0xC2, DM1DAD_H);
    break;
  case 0xC4:
  	DM1CNT_L = value & 0x3FFF;
    UPDATE_REG(0xC4, 0);
    break;
  case 0xC6: {
      bool start = ((DM1CNT_H ^ value) & 0x8000) ? true : false;
      value &= 0xF7E0;

      DM1CNT_H = value;
      UPDATE_REG(0xC6, DM1CNT_H);

      if (start && (value & 0x8000)) {
        dma1Source = DM1SAD_L | (DM1SAD_H << 16);
        dma1Dest = DM1DAD_L | (DM1DAD_H << 16);
        CPUCheckDMA(0, 2);
      }
    } break;
  case 0xC8:
  	DM2SAD_L = value;
    UPDATE_REG(0xC8, DM2SAD_L);
    break;
  case 0xCA:
  	DM2SAD_H = value & 0x0FFF;
    UPDATE_REG(0xCA, DM2SAD_H);
    break;
  case 0xCC:
  	DM2DAD_L = value;
    UPDATE_REG(0xCC, DM2DAD_L);
    break;
  case 0xCE:
  	DM2DAD_H = value & 0x07FF;
    UPDATE_REG(0xCE, DM2DAD_H);
    break;
  case 0xD0:
  	DM2CNT_L = value & 0x3FFF;
    UPDATE_REG(0xD0, 0);
    break;
  case 0xD2: {
      bool start = ((DM2CNT_H ^ value) & 0x8000) ? true : false;

      value &= 0xF7E0;

      DM2CNT_H = value;
      UPDATE_REG(0xD2, DM2CNT_H);

      if (start && (value & 0x8000)) {
        dma2Source = DM2SAD_L | (DM2SAD_H << 16);
        dma2Dest = DM2DAD_L | (DM2DAD_H << 16);

        CPUCheckDMA(0, 4);
      }
    } break;
  case 0xD4:
  	DM3SAD_L = value;
    UPDATE_REG(0xD4, DM3SAD_L);
    break;
  case 0xD6:
  	DM3SAD_H = value & 0x0FFF;
    UPDATE_REG(0xD6, DM3SAD_H);
    break;
  case 0xD8:
  	DM3DAD_L = value;
    UPDATE_REG(0xD8, DM3DAD_L);
    break;
  case 0xDA:
  	DM3DAD_H = value & 0x0FFF;
    UPDATE_REG(0xDA, DM3DAD_H);
    break;
  case 0xDC:
  	DM3CNT_L = value;
    UPDATE_REG(0xDC, 0);
    break;
  case 0xDE: {
      bool start = ((DM3CNT_H ^ value) & 0x8000) ? true : false;

      value &= 0xFFE0;

      DM3CNT_H = value;
      UPDATE_REG(0xDE, DM3CNT_H);

      if (start && (value & 0x8000)) {
        dma3Source = DM3SAD_L | (DM3SAD_H << 16);
        dma3Dest = DM3DAD_L | (DM3DAD_H << 16);
        CPUCheckDMA(0, 8);
      }
    } break;
  case 0x100:
    timer0Reload = value;
    interp_rate();
    break;
  case 0x102:
    timer0Value = value;
    timerOnOffDelay |= 1;
    cpuNextEvent = cpuTotalTicks;
    break;
  case 0x104:
    timer1Reload = value;
    interp_rate();
    break;
  case 0x106:
    timer1Value = value;
    timerOnOffDelay |= 2;
    cpuNextEvent = cpuTotalTicks;
    break;
  case 0x108:
    timer2Reload = value;
    break;
  case 0x10A:
    timer2Value = value;
    timerOnOffDelay |= 4;
    cpuNextEvent = cpuTotalTicks;
    break;
  case 0x10C:
    timer3Reload = value;
    break;
  case 0x10E:
    timer3Value = value;
    timerOnOffDelay |= 8;
    cpuNextEvent = cpuTotalTicks;
    break;

  case COMM_SIOCNT:
#ifndef NO_LINK
    StartLink(value);
#else
    if (value & 0x80) {
        value &= 0xff7f;
        if (value & 1 && (value & 0x4000)) {
            UPDATE_REG(cpu.gba, COMM_SIOCNT, 0xFF);
            IF |= 0x80;
            UPDATE_REG(0x202, IF);
            value &= 0x7f7f;
        }
    }
    UPDATE_REG(cpu.gba, COMM_SIOCNT, value);
#endif
  break;

#ifndef NO_LINK
  case COMM_SIODATA8:
	  UPDATE_REG(cpu.gba, COMM_SIODATA8, value);
	  break;
#endif

  case 0x130:
	  P1 |= (value & 0x3FF);
	  UPDATE_REG(0x130, P1);
	  break;

  case 0x132:
	  UPDATE_REG(cpu.gba, 0x132, value & 0xC3FF);
	  break;

  case COMM_RCNT:
#ifndef NO_LINK
    StartGPLink(value);
#else
  	UPDATE_REG(cpu.gba, COMM_RCNT, value);
#endif
   break;

#ifndef NO_LINK
  case COMM_JOYCNT: {
		  uint16_t cur = READ16LE(&ioMem.b[COMM_JOYCNT]);

      if (value & JOYCNT_RESET)
          cur &= ~JOYCNT_RESET;
      if (value & JOYCNT_RECV_COMPLETE)
          cur &= ~JOYCNT_RECV_COMPLETE;
      if (value & JOYCNT_SEND_COMPLETE)
          cur &= ~JOYCNT_SEND_COMPLETE;
      if (value & JOYCNT_INT_ENABLE)
          cur |= JOYCNT_INT_ENABLE;

		  UPDATE_REG(cpu.gba, COMM_JOYCNT, cur);
	  } break;

  case COMM_JOY_RECV_L:
	  UPDATE_REG(cpu.gba, COMM_JOY_RECV_L, value);
	  break;
  case COMM_JOY_RECV_H:
	  UPDATE_REG(cpu.gba, COMM_JOY_RECV_H, value);
	  break;

  case COMM_JOY_TRANS_L:
	  UPDATE_REG(cpu.gba, COMM_JOY_TRANS_L, value);
	  UPDATE_REG(cpu.gba, COMM_JOYSTAT, READ16LE(&ioMem.b[COMM_JOYSTAT]) | JOYSTAT_SEND);
	  break;
  case COMM_JOY_TRANS_H:
  	UPDATE_REG(cpu.gba, COMM_JOY_TRANS_H, value);
  	UPDATE_REG(cpu.gba, COMM_JOYSTAT, READ16LE(&ioMem.b[COMM_JOYSTAT]) | JOYSTAT_SEND);
	  break;

  case COMM_JOYSTAT:
  	UPDATE_REG(cpu.gba, COMM_JOYSTAT, (READ16LE(&ioMem[COMM_JOYSTAT]) & 0x0a) | (value & ~0x0a));
	  break;
#endif

  case 0x200:
    IE = value & 0x3FFF;
    UPDATE_REG(0x200, IE);
    if ((IME & 1) && (IF & IE) && armIrqEnable)
      cpuNextEvent = cpuTotalTicks;
    break;
  case 0x202:
    IF ^= (value & IF);
    UPDATE_REG(0x202, IF);
    break;
  case 0x204: {
    	memoryWait[0x0e] = memoryWaitSeq[0x0e] = gamepakRamWaitState[value & 3];

      if (!coreOptions.speedHack) {
      	memoryWait[0x08] = memoryWait[0x09] = gamepakWaitState[(value >> 2) & 3];
      	memoryWaitSeq[0x08] = memoryWaitSeq[0x09] = gamepakWaitState0[(value >> 4) & 1];

      	memoryWait[0x0a] = memoryWait[0x0b] = gamepakWaitState[(value >> 5) & 3];
      	memoryWaitSeq[0x0a] = memoryWaitSeq[0x0b] = gamepakWaitState1[(value >> 7) & 1];

      	memoryWait[0x0c] = memoryWait[0x0d] = gamepakWaitState[(value >> 8) & 3];
      	memoryWaitSeq[0x0c] = memoryWaitSeq[0x0d] = gamepakWaitState2[(value >> 10) & 1];
      } else {
      	memoryWait[0x08] = memoryWait[0x09] = 3;
      	memoryWaitSeq[0x08] = memoryWaitSeq[0x09] = 1;

      	memoryWait[0x0a] = memoryWait[0x0b] = 3;
      	memoryWaitSeq[0x0a] = memoryWaitSeq[0x0b] = 1;

      	memoryWait[0x0c] = memoryWait[0x0d] = 3;
      	memoryWaitSeq[0x0c] = memoryWaitSeq[0x0d] = 1;
      }

      for (int i = 8; i < 15; i++) {
      	memoryWait32[i] = memoryWait[i] + memoryWaitSeq[i] + 1;
      	memoryWaitSeq32[i] = memoryWaitSeq[i] * 2 + 1;
      }

      if ((value & 0x4000) == 0x4000) {
        busPrefetchEnable = true;
        busPrefetch = false;
        busPrefetchCount = 0;
      } else {
        busPrefetchEnable = false;
        busPrefetch = false;
        busPrefetchCount = 0;
      }
      UPDATE_REG(cpu.gba, 0x204, value & 0x7FFF);

    } break;
  case 0x208:
    IME = value & 1;
    UPDATE_REG(0x208, IME);
    if ((IME & 1) && (IF & IE) && armIrqEnable)
      cpuNextEvent = cpuTotalTicks;
    break;
  case 0x300:
    if (value != 0)
      value &= 0xFFFE;
    UPDATE_REG(cpu.gba, 0x300, value);
    break;
  default:
    UPDATE_REG(cpu.gba, address&0x3FE, value);
    break;
  }
}

void applyTimer(GBASys &gba, ARM7TDMI &cpu)
{
	auto &ioMem = gba.mem.ioMem;

  if (timerOnOffDelay & 1) {
    timer0ClockReload = TIMER_TICKS[timer0Value & 3];
    if (!timer0On && (timer0Value & 0x80)) {
      // reload the counter
    	TM0D = timer0Reload;
      timer0Ticks = (0x10000 - TM0D) << timer0ClockReload;
      UPDATE_REG(0x100, TM0D);
    }
    timer0On = timer0Value & 0x80 ? true : false;
    TM0CNT = timer0Value & 0xC7;
    interp_rate();
    UPDATE_REG(0x102, TM0CNT);
    //    CPUUpdateTicks();
  }
  if (timerOnOffDelay & 2) {
    timer1ClockReload = TIMER_TICKS[timer1Value & 3];
    if (!timer1On && (timer1Value & 0x80)) {
      // reload the counter
    	TM1D = timer1Reload;
      timer1Ticks = (0x10000 - TM1D) << timer1ClockReload;
      UPDATE_REG(0x104, TM1D);
    }
    timer1On = timer1Value & 0x80 ? true : false;
    TM1CNT = timer1Value & 0xC7;
    interp_rate();
    UPDATE_REG(0x106, TM1CNT);
  }
  if (timerOnOffDelay & 4) {
    timer2ClockReload = TIMER_TICKS[timer2Value & 3];
    if (!timer2On && (timer2Value & 0x80)) {
      // reload the counter
    	TM2D = timer2Reload;
      timer2Ticks = (0x10000 - TM2D) << timer2ClockReload;
      UPDATE_REG(0x108, TM2D);
    }
    timer2On = timer2Value & 0x80 ? true : false;
    TM2CNT = timer2Value & 0xC7;
    UPDATE_REG(0x10A, TM2CNT);
  }
  if (timerOnOffDelay & 8) {
    timer3ClockReload = TIMER_TICKS[timer3Value & 3];
    if (!timer3On && (timer3Value & 0x80)) {
      // reload the counter
    	TM3D = timer3Reload;
      timer3Ticks = (0x10000 - TM3D) << timer3ClockReload;
      UPDATE_REG(0x10C, TM3D);
    }
    timer3On = timer3Value & 0x80 ? true : false;
    TM3CNT = timer3Value & 0xC7;
    UPDATE_REG(0x10E, TM3CNT);
  }
  cpuNextEvent = CPUUpdateTicks();
  timerOnOffDelay = 0;
}

void CPUInit(GBASys &gba, const char *biosFileName, bool useBiosFile)
{
#ifdef WORDS_BIGENDIAN
  if (!cpuBiosSwapped) {
    for (unsigned int i = 0; i < sizeof(myROM) / 4; i++) {
      WRITE32LE(&myROM[i], myROM[i]);
    }
    cpuBiosSwapped = true;
  }
#endif
  eepromInUse = 0;
  coreOptions.useBios = false;

#if 0
  if (useBiosFile && strlen(biosFileName) > 0) {
    int size = 0x4000;
    if (utilLoad(biosFileName,
                CPUIsGBABios,
                bios,
                size)) {
      if (size == 0x4000)
        coreOptions.useBios = true;
      else
        systemMessage(MSG_INVALID_BIOS_FILE_SIZE, N_("Invalid BIOS file size"));
    }
  }
#endif

  if (!coreOptions.useBios) {
    memcpy(bios, myROM, sizeof(myROM));
  }

  int i = 0;

  biosProtected[0] = 0x00;
  biosProtected[1] = 0xf0;
  biosProtected[2] = 0x29;
  biosProtected[3] = 0xe1;

  /*
  for (i = 0; i < 256; i++) {
    int count = 0;
    int j;
    for (j = 0; j < 8; j++)
      if (i & (1 << j))
        count++;
    cpuBitsSet[i] = count;

    for (j = 0; j < 8; j++)
      if (i & (1 << j))
        break;
    cpuLowestBitSet[i] = j;
  }

  for (i = 0; i < 0x400; i++)
    ioReadable[i] = true;
  for (i = 0x10; i < 0x48; i++)
    ioReadable[i] = false;
  for (i = 0x4c; i < 0x50; i++)
    ioReadable[i] = false;
  for (i = 0x54; i < 0x60; i++)
    ioReadable[i] = false;
  for (i = 0x8c; i < 0x90; i++)
    ioReadable[i] = false;
  for (i = 0xa0; i < 0xb8; i++)
    ioReadable[i] = false;
  for (i = 0xbc; i < 0xc4; i++)
    ioReadable[i] = false;
  for (i = 0xc8; i < 0xd0; i++)
    ioReadable[i] = false;
  for (i = 0xd4; i < 0xdc; i++)
    ioReadable[i] = false;
  for (i = 0xe0; i < 0x100; i++)
    ioReadable[i] = false;
  for (i = 0x110; i < 0x120; i++)
    ioReadable[i] = false;
  for (i = 0x12c; i < 0x130; i++)
    ioReadable[i] = false;
  for (i = 0x138; i < 0x140; i++)
    ioReadable[i] = false;
  for (i = 0x144; i < 0x150; i++)
    ioReadable[i] = false;
  for (i = 0x15c; i < 0x200; i++)
    ioReadable[i] = false;
  for (i = 0x20c; i < 0x300; i++)
    ioReadable[i] = false;
  for (i = 0x304; i < 0x400; i++)
    ioReadable[i] = false;*/

  gba.cpu.map = gbaMap;

  if (romSize < 0x1fe2000) {
  	*((uint16_t*)&rom[0x1fe209c]) = 0xdffa; // SWI 0xFA
  	*((uint16_t*)&rom[0x1fe209e]) = 0x4770; // BX LR
  } else {
    agbPrintEnable(false);
  }
}

void SetSaveType(int st)
{
    switch (st) {
    case GBA_SAVE_AUTO:
        cpuSramEnabled = true;
        cpuFlashEnabled = true;
        cpuEEPROMEnabled = true;
        cpuEEPROMSensorEnabled = false;
        cpuSaveGameFunc = flashSaveDecide;
        break;
    case GBA_SAVE_EEPROM:
        cpuSramEnabled = false;
        cpuFlashEnabled = false;
        cpuEEPROMEnabled = true;
        cpuEEPROMSensorEnabled = false;
        break;
    case GBA_SAVE_SRAM:
        cpuSramEnabled = true;
        cpuFlashEnabled = false;
        cpuEEPROMEnabled = false;
        cpuEEPROMSensorEnabled = false;
        cpuSaveGameFunc = sramDelayedWrite; // to insure we detect the write
        break;
    case GBA_SAVE_FLASH:
        cpuSramEnabled = false;
        cpuFlashEnabled = true;
        cpuEEPROMEnabled = false;
        cpuEEPROMSensorEnabled = false;
        cpuSaveGameFunc = flashDelayedWrite; // to insure we detect the write
        break;
    case GBA_SAVE_EEPROM_SENSOR:
        cpuSramEnabled = false;
        cpuFlashEnabled = false;
        cpuEEPROMEnabled = true;
        cpuEEPROMSensorEnabled = true;
        break;
    case GBA_SAVE_NONE:
        cpuSramEnabled = false;
        cpuFlashEnabled = false;
        cpuEEPROMEnabled = false;
        cpuEEPROMSensorEnabled = false;
        break;
    }
}

void CPUReset(GBASys &gba)
{
	auto &cpu = gba.cpu;
  switch (CheckEReaderRegion()) {
  case 1: //US
      EReaderWriteMemory(0x8009134, 0x46C0DFE0);
      break;
  case 2:
      EReaderWriteMemory(0x8008A8C, 0x46C0DFE0);
      break;
  case 3:
      EReaderWriteMemory(0x80091A8, 0x46C0DFE0);
      break;
  }
  rtcReset();
  // clean io memory
  memset(gba.mem.ioMem.b, 0, 0x400);
  // clean OAM, palette, picture, & vram
  gba.lcd.resetAll(coreOptions.useBios, coreOptions.skipBios, gba.mem.ioMem);

#if 0
  DISPCNT = 0x0080;
  DISPSTAT = 0x0000;
  VCOUNT = (coreOptions.useBios && !coreOptions.skipBios) ? 0 : 0x007E;
  BG0CNT = 0x0000;
  BG1CNT = 0x0000;
  BG2CNT = 0x0000;
  BG3CNT = 0x0000;
  BG0HOFS = 0x0000;
  BG0VOFS = 0x0000;
  BG1HOFS = 0x0000;
  BG1VOFS = 0x0000;
  BG2HOFS = 0x0000;
  BG2VOFS = 0x0000;
  BG3HOFS = 0x0000;
  BG3VOFS = 0x0000;
  BG2PA = 0x0100;
  BG2PB = 0x0000;
  BG2PC = 0x0000;
  BG2PD = 0x0100;
  BG2X_L = 0x0000;
  BG2X_H = 0x0000;
  BG2Y_L = 0x0000;
  BG2Y_H = 0x0000;
  BG3PA = 0x0100;
  BG3PB = 0x0000;
  BG3PC = 0x0000;
  BG3PD = 0x0100;
  BG3X_L = 0x0000;
  BG3X_H = 0x0000;
  BG3Y_L = 0x0000;
  BG3Y_H = 0x0000;
  WIN0H = 0x0000;
  WIN1H = 0x0000;
  WIN0V = 0x0000;
  WIN1V = 0x0000;
  WININ = 0x0000;
  WINOUT = 0x0000;
  MOSAIC = 0x0000;
  BLDMOD = 0x0000;
  COLEV = 0x0000;
  COLY = 0x0000;
  DM0SAD_L = 0x0000;
  DM0SAD_H = 0x0000;
  DM0DAD_L = 0x0000;
  DM0DAD_H = 0x0000;
  DM0CNT_L = 0x0000;
  DM0CNT_H = 0x0000;
  DM1SAD_L = 0x0000;
  DM1SAD_H = 0x0000;
  DM1DAD_L = 0x0000;
  DM1DAD_H = 0x0000;
  DM1CNT_L = 0x0000;
  DM1CNT_H = 0x0000;
  DM2SAD_L = 0x0000;
  DM2SAD_H = 0x0000;
  DM2DAD_L = 0x0000;
  DM2DAD_H = 0x0000;
  DM2CNT_L = 0x0000;
  DM2CNT_H = 0x0000;
  DM3SAD_L = 0x0000;
  DM3SAD_H = 0x0000;
  DM3DAD_L = 0x0000;
  DM3DAD_H = 0x0000;
  DM3CNT_L = 0x0000;
  DM3CNT_H = 0x0000;
#endif
  gba.lcd.lineMix = &gba.lcd.pix[240 * VCOUNT];
  gba.dma.reset(gba.mem.ioMem);
  TM0D     = 0x0000;
  TM0CNT   = 0x0000;
  TM1D     = 0x0000;
  TM1CNT   = 0x0000;
  TM2D     = 0x0000;
  TM2CNT   = 0x0000;
  TM3D     = 0x0000;
  TM3CNT   = 0x0000;
  P1       = 0x03FF;
#if 0
  IE = 0x0000;
  IF = 0x0000;
  IME = 0x0000;

  armMode = 0x1F;

  if (coreOptions.cpuIsMultiBoot) {
      reg[13].I = 0x03007F00;
      reg[15].I = 0x02000000;
      reg[16].I = 0x00000000;
      reg[R13_IRQ].I = 0x03007FA0;
      reg[R13_SVC].I = 0x03007FE0;
      armIrqEnable = true;
  } else {
      if (coreOptions.useBios && !coreOptions.skipBios) {
          reg[15].I = 0x00000000;
          armMode = 0x13;
          armIrqEnable = false;
      } else {
          reg[13].I = 0x03007F00;
          reg[15].I = 0x08000000;
          reg[16].I = 0x00000000;
          reg[R13_IRQ].I = 0x03007FA0;
          reg[R13_SVC].I = 0x03007FE0;
          armIrqEnable = true;
      }
  }
  armState = true;
  C_FLAG = V_FLAG = N_FLAG = Z_FLAG = false;
#endif
  gba.cpu.reset(gba.mem.ioMem, coreOptions.cpuIsMultiBoot, coreOptions.useBios, coreOptions.skipBios);

  UPDATE_REG(0x00, DISPCNT);
  UPDATE_REG(0x06, VCOUNT);
  UPDATE_REG(0x20, BG2PA);
  UPDATE_REG(0x26, BG2PD);
  UPDATE_REG(0x30, BG3PA);
  UPDATE_REG(0x36, BG3PD);
  UPDATE_REG(0x130, P1);
  UPDATE_REG(&gba, 0x88, 0x200);

  holdType = 0;

  biosProtected[0] = 0x00;
  biosProtected[1] = 0xf0;
  biosProtected[2] = 0x29;
  biosProtected[3] = 0xe1;

  gba.lcd.lcdTicks = (coreOptions.useBios && !coreOptions.skipBios) ? 1008 : 208;
  timer0On = false;
  timer0Ticks = 0;
  timer0Reload = 0;
  timer0ClockReload  = 0;
  timer1On = false;
  timer1Ticks = 0;
  timer1Reload = 0;
  timer1ClockReload  = 0;
  timer2On = false;
  timer2Ticks = 0;
  timer2Reload = 0;
  timer2ClockReload  = 0;
  timer3On = false;
  timer3Ticks = 0;
  timer3Reload = 0;
  timer3ClockReload  = 0;
  cpuSaveGameFunc = flashSaveDecide;
  gba.lcd.renderLine = mode0RenderLine;
  fxOn = false;
  windowOn = false;

  CPUUpdateRenderBuffers(true);

  /*for (int i = 0; i < 256; i++) {
    map[i].address = (uint8_t*)&dummyAddress;
    map[i].mask = 0;
  }

  map[0].address = bios;
  map[2].address = workRAM;
  map[3].address = internalRAM;
  map[4].address = ioMem;
  map[5].address = paletteRAM;
  map[6].address = vram;
  map[7].address = oam;
  map[8].address = rom;
  map[9].address = rom;
  map[10].address = rom;
  map[12].address = rom;
  map[14].address = flashSaveMemory;

  SetMapMasks();*/

  eepromReset();
  flashReset();

  soundReset(gba);

  CPUUpdateWindow0();
  CPUUpdateWindow1();

  // make sure registers are correctly initialized if not using BIOS
  if (!coreOptions.useBios) {
    if (coreOptions.cpuIsMultiBoot)
      BIOS_RegisterRamReset(gba.cpu, 0xfe);
    else
      BIOS_RegisterRamReset(gba.cpu, 0xff);
  } else {
    if (coreOptions.cpuIsMultiBoot)
      BIOS_RegisterRamReset(gba.cpu, 0xfe);
  }

  flashReset();
  eepromReset();
  SetSaveType(coreOptions.saveType);

  gba.cpu.ARM_PREFETCH();

  systemSaveUpdateCounter = SYSTEM_SAVE_NOT_UPDATED;

  cpuDmaRunning = false;

  lastTime = systemGetClock();

  SWITicks = 0;
}

void ARM7TDMI::interrupt(const GBAMem::IoMem &ioMem)
{
	auto &cpu = *this;
	uint32_t PC = reg[15].I;
	bool savedState = armState;
	switchMode(ioMem, 0x12, true, false);
	reg[14].I = PC;
	if (!savedState)
		reg[14].I += 2;
	reg[15].I = 0x18;
	armState = true;
	armIrqEnable = false;

	armNextPC = reg[15].I;
	reg[15].I += 4;
	ARM_PREFETCH();
}

void CPUInterrupt(GBASys &gba, ARM7TDMI &cpu)
{
	cpu.interrupt(gba.mem.ioMem);

  //  if(!holdState)
	biosProtected[0] = 0x02;
	biosProtected[1] = 0xc0;
	biosProtected[2] = 0x5e;
	biosProtected[3] = 0xe5;
}

#define CPUInterrupt() CPUInterrupt(gba, cpu)

static uint32_t joy;
static bool has_frames;

static void gbaUpdateJoypads(GBASys &gba)
{
    auto &ioMem = gba.mem.ioMem.b;
#if 0
    // update joystick information
    if (systemReadJoypads())
        // read default joystick
        joy = systemReadJoypad(-1);

    P1 = 0x03FF ^ (joy & 0x3FF);
    systemUpdateMotionSensor();
    UPDATE_REG(0x130, P1);
#endif
    uint16_t P1CNT = READ16LE(((uint16_t*)&ioMem[0x132]));

    // this seems wrong, but there are cases where the game
    // can enter the stop state without requesting an IRQ from
    // the joypad.
    if ((P1CNT & 0x4000) || stopState) {
        uint16_t p1 = (0x3FF ^ P1) & 0x3FF;
        if (P1CNT & 0x8000) {
            if (p1 == (P1CNT & 0x3FF)) {
                IF |= 0x1000;
                UPDATE_REG(0x202, IF);
            }
        } else {
            if (p1 & P1CNT) {
                IF |= 0x1000;
                UPDATE_REG(0x202, IF);
            }
        }
    }
}

void CPULoop(GBASys &gba, EmuEx::EmuSystemTaskContext taskCtx, EmuEx::EmuVideo *video, EmuEx::EmuAudio *audio)
{
	auto cpu = gba.cpu;
	auto restoreCpu = IG::scopeGuard([&](){ gba.cpu = cpu; });
	auto &holdState = cpu.holdState;
	auto &armIrqEnable = cpu.armIrqEnable;
	auto &ioMem = gba.mem.ioMem;
	auto &IE = ioMem.IE;
	auto &IME = ioMem.IME;
	auto &lcdTicks = gba.lcd.lcdTicks;
	const int ticks = 300000;
  int clockTicks;
  int timerOverflow = 0;
  // variable used by the CPU core
  cpuTotalTicks = 0;

#ifndef NO_LINK
// shuffle2: what's the purpose?
//if(GetLinkMode() != LINK_DISCONNECTED)
//cpuNextEvent = 1;
#endif

  gbaUpdateJoypads(gba);
  cpuNextEvent = CPUUpdateTicks();
  /*if (cpuNextEvent > ticks)
    cpuNextEvent = ticks;*/

  for (;;) {
    if (!holdState && !SWITicks) {
      if (armState) {
      	armOpcodeCount++;
        if (!armExecute(cpu))
        {
#ifdef BKPT_SUPPORT
          return;
#endif
        }
        if (debugger)
        	return;
      } else {
      	thumbOpcodeCount++;
        if (!thumbExecute(cpu))
        {
#ifdef BKPT_SUPPORT
          return;
#endif
        }
        if (debugger)
        	return;
      }
      clockTicks = 0;
    } else
      clockTicks = CPUUpdateTicks();

    cpuTotalTicks += clockTicks;

    if (rtcIsEnabled())
        rtcUpdateTime(cpuTotalTicks);

    if (cpuTotalTicks >= cpuNextEvent) {
    	bool cpuBreakLoop = false;
      int remainingTicks = cpuTotalTicks - cpuNextEvent;

      if (SWITicks) {
        SWITicks -= clockTicks;
        if (SWITicks < 0)
          SWITicks = 0;
      }

      clockTicks = cpuNextEvent;
      cpuTotalTicks = 0;

    updateLoop:

      if (IRQTicks) {
          IRQTicks -= clockTicks;
        if (IRQTicks < 0)
          IRQTicks = 0;
      }

      lcdTicks -= clockTicks;

      soundTicks += clockTicks;

      if (lcdTicks <= 0) {
        if (DISPSTAT & 1) { // V-BLANK
          // if in V-Blank mode, keep computing...
          if (DISPSTAT & 2) {
          	lcdTicks += 1008;
            VCOUNT++;
            UPDATE_REG(0x06, VCOUNT);
            DISPSTAT &= 0xFFFD;
            UPDATE_REG(0x04, DISPSTAT);
            CPUCompareVCOUNT();
          } else {
          	lcdTicks += 224;
            DISPSTAT |= 2;
            UPDATE_REG(0x04, DISPSTAT);
            if (DISPSTAT & 16) {
              IF |= 2;
              UPDATE_REG(0x202, IF);
            }
          }

          if (VCOUNT > 227) { //Reaching last line
          	DISPSTAT &= 0xFFFC;
            UPDATE_REG(0x04, DISPSTAT);
          	VCOUNT = 0;
          	gba.lcd.lineMix = gba.lcd.pix;
            UPDATE_REG(0x06, VCOUNT);
            CPUCompareVCOUNT();
          }
        } else {
        	int framesToSkip = systemFrameSkip;

					static bool speedup_throttle_set = false;
          bool turbo_button_pressed        = (joy >> 10) & 1;
#if 0
					static uint32_t last_throttle;

					if (turbo_button_pressed) {
							if (coreOptions.speedup_frame_skip)
									framesToSkip = coreOptions.speedup_frame_skip;
							else {
									if (!speedup_throttle_set && coreOptions.throttle != coreOptions.speedup_throttle) {
											last_throttle = coreOptions.throttle;
											soundSetThrottle(coreOptions.speedup_throttle);
											speedup_throttle_set = true;
									}

									if (coreOptions.speedup_throttle_frame_skip)
											framesToSkip += std::ceil(double(coreOptions.speedup_throttle) / 100.0) - 1;
							}
					}
					else if (speedup_throttle_set) {
							soundSetThrottle(last_throttle);
							speedup_throttle_set = false;
					}
#else
					if (turbo_button_pressed)
							framesToSkip = 9;
#endif

          if (DISPSTAT & 2) {
            // if in H-Blank, leave it and move to drawing mode
          	VCOUNT++;
          	gba.lcd.lineMix += 240;
            UPDATE_REG(0x06, VCOUNT);

          	lcdTicks += 1008;
            DISPSTAT &= 0xFFFD;
            if (VCOUNT == 160) {
#if 0
              count++;
              systemFrame();

              if ((count % 10) == 0) {
                  system10Frames();
              }
              if (count == 60) {
                  uint32_t time = systemGetClock();
                  if (time != lastTime) {
                      uint32_t t = 100000 / (time - lastTime);
                      systemShowSpeed(t);
                  } else
                      systemShowSpeed(0);
                  lastTime = time;
                  count = 0;
              }
#endif

              uint32_t ext = (joy >> 10);
              // If no (m) code is enabled, apply the cheats at each LCDline
              if (cheatsList.size())
              	remainingTicks += cheatsCheckKeys(cpu, P1 ^ 0x3FF, 0);

#if 0
              coreOptions.speedup = false;

              if (ext & 1 && !speedup_throttle_set)
                  coreOptions.speedup = true;

              capture = (ext & 2) ? true : false;

              if (capture && !capturePrevious) {
                  captureNumber++;
                  systemScreenCapture(captureNumber);
              }
              capturePrevious = capture;
#endif

              DISPSTAT |= 1;
              DISPSTAT &= 0xFFFD;
              UPDATE_REG(0x04, DISPSTAT);
              if (DISPSTAT & 0x0008) {
                IF |= 1;
                UPDATE_REG(0x202, IF);
              }
              CPUCheckDMA(1, 0x0f);

              //psoundTickfn(audio);

#if 0
              if (frameCount >= framesToSkip) {
                  systemDrawScreen();
                  frameCount = 0;
              } else {
                  frameCount++;
                  systemSendScreen();
              }
              if (systemPauseOnFrame())
                  ticks = 0;
#endif
            }

            UPDATE_REG(0x04, DISPSTAT);
            CPUCompareVCOUNT();

          } else {
#if 0
            if (frameCount >= framesToSkip) {
                (*renderLine)();
                switch (systemColorDepth) {
                case 16: {
#ifdef __LIBRETRO__
                    uint16_t* dest = (uint16_t*)pix + 240 * VCOUNT;
#else
                    uint16_t* dest = (uint16_t*)pix + 242 * (VCOUNT + 1);
#endif
                    for (int x = 0; x < 240;) {
                        *dest++ = systemColorMap16[lineMix[x++] & 0xFFFF];
                        *dest++ = systemColorMap16[lineMix[x++] & 0xFFFF];
                        *dest++ = systemColorMap16[lineMix[x++] & 0xFFFF];
                        *dest++ = systemColorMap16[lineMix[x++] & 0xFFFF];

                        *dest++ = systemColorMap16[lineMix[x++] & 0xFFFF];
                        *dest++ = systemColorMap16[lineMix[x++] & 0xFFFF];
                        *dest++ = systemColorMap16[lineMix[x++] & 0xFFFF];
                        *dest++ = systemColorMap16[lineMix[x++] & 0xFFFF];

                        *dest++ = systemColorMap16[lineMix[x++] & 0xFFFF];
                        *dest++ = systemColorMap16[lineMix[x++] & 0xFFFF];
                        *dest++ = systemColorMap16[lineMix[x++] & 0xFFFF];
                        *dest++ = systemColorMap16[lineMix[x++] & 0xFFFF];

                        *dest++ = systemColorMap16[lineMix[x++] & 0xFFFF];
                        *dest++ = systemColorMap16[lineMix[x++] & 0xFFFF];
                        *dest++ = systemColorMap16[lineMix[x++] & 0xFFFF];
                        *dest++ = systemColorMap16[lineMix[x++] & 0xFFFF];
                    }
// for filters that read past the screen
#ifndef __LIBRETRO__
                    *dest++ = 0;
#endif
                } break;
                case 24: {
                    uint8_t* dest = (uint8_t*)pix + 240 * VCOUNT * 3;
                    for (int x = 0; x < 240;) {
                        *((uint32_t*)dest) = systemColorMap32[lineMix[x++] & 0xFFFF];
                        dest += 3;
                        *((uint32_t*)dest) = systemColorMap32[lineMix[x++] & 0xFFFF];
                        dest += 3;
                        *((uint32_t*)dest) = systemColorMap32[lineMix[x++] & 0xFFFF];
                        dest += 3;
                        *((uint32_t*)dest) = systemColorMap32[lineMix[x++] & 0xFFFF];
                        dest += 3;

                        *((uint32_t*)dest) = systemColorMap32[lineMix[x++] & 0xFFFF];
                        dest += 3;
                        *((uint32_t*)dest) = systemColorMap32[lineMix[x++] & 0xFFFF];
                        dest += 3;
                        *((uint32_t*)dest) = systemColorMap32[lineMix[x++] & 0xFFFF];
                        dest += 3;
                        *((uint32_t*)dest) = systemColorMap32[lineMix[x++] & 0xFFFF];
                        dest += 3;

                        *((uint32_t*)dest) = systemColorMap32[lineMix[x++] & 0xFFFF];
                        dest += 3;
                        *((uint32_t*)dest) = systemColorMap32[lineMix[x++] & 0xFFFF];
                        dest += 3;
                        *((uint32_t*)dest) = systemColorMap32[lineMix[x++] & 0xFFFF];
                        dest += 3;
                        *((uint32_t*)dest) = systemColorMap32[lineMix[x++] & 0xFFFF];
                        dest += 3;

                        *((uint32_t*)dest) = systemColorMap32[lineMix[x++] & 0xFFFF];
                        dest += 3;
                        *((uint32_t*)dest) = systemColorMap32[lineMix[x++] & 0xFFFF];
                        dest += 3;
                        *((uint32_t*)dest) = systemColorMap32[lineMix[x++] & 0xFFFF];
                        dest += 3;
                        *((uint32_t*)dest) = systemColorMap32[lineMix[x++] & 0xFFFF];
                        dest += 3;
                    }
                } break;
                case 32: {
#ifdef __LIBRETRO__
                    uint32_t* dest = (uint32_t*)pix + 240 * VCOUNT;
#else
                    uint32_t* dest = (uint32_t*)pix + 241 * (VCOUNT + 1);
#endif
                    for (int x = 0; x < 240;) {
                        *dest++ = systemColorMap32[lineMix[x++] & 0xFFFF];
                        *dest++ = systemColorMap32[lineMix[x++] & 0xFFFF];
                        *dest++ = systemColorMap32[lineMix[x++] & 0xFFFF];
                        *dest++ = systemColorMap32[lineMix[x++] & 0xFFFF];

                        *dest++ = systemColorMap32[lineMix[x++] & 0xFFFF];
                        *dest++ = systemColorMap32[lineMix[x++] & 0xFFFF];
                        *dest++ = systemColorMap32[lineMix[x++] & 0xFFFF];
                        *dest++ = systemColorMap32[lineMix[x++] & 0xFFFF];

                        *dest++ = systemColorMap32[lineMix[x++] & 0xFFFF];
                        *dest++ = systemColorMap32[lineMix[x++] & 0xFFFF];
                        *dest++ = systemColorMap32[lineMix[x++] & 0xFFFF];
                        *dest++ = systemColorMap32[lineMix[x++] & 0xFFFF];

                        *dest++ = systemColorMap32[lineMix[x++] & 0xFFFF];
                        *dest++ = systemColorMap32[lineMix[x++] & 0xFFFF];
                        *dest++ = systemColorMap32[lineMix[x++] & 0xFFFF];
                        *dest++ = systemColorMap32[lineMix[x++] & 0xFFFF];
                    }
                } break;
                }
            }
#endif
            if (video) [[likely]]
            {
            	/*if (trackOAM)
            	{
            		if (oamUpdated)
            		{
            			//systemMessage(0, "OAM updated, line %d", (int)VCOUNT);
            			//oamUpdated = 0;
            		}
            	}
            	else
            	{
            	}*/
              (*gba.lcd.renderLine)(gba.lcd.lineMix, gba.lcd, ioMem);
            }
            if (VCOUNT == 159)
            {
            	cpuBreakLoop = true;
              if (video)
              {
            	  systemDrawScreen(taskCtx, *video);
            	  video = nullptr;
              }
            }
            if (VCOUNT % 53 == 0) // flush audio samples 4 times per frame
              psoundTickfn(audio);
            // entering H-Blank
            DISPSTAT |= 2;
            UPDATE_REG(0x04, DISPSTAT);
            lcdTicks += 224;
            CPUCheckDMA(2, 0x0f);
            if (DISPSTAT & 16) {
              IF |= 2;
              UPDATE_REG(0x202, IF);
            }
          }
        }
      }

	    // we shouldn't be doing sound in stop state, but we loose synchronization
      // if sound is disabled, so in stop state, soundTick will just produce
      // mute sound
      //soundTicks -= clockTicks;
      //if (soundTicks <= 0) {
        //psoundTickfn();
        //soundTicks += SOUND_CLOCK_TICKS;
      //}

      if (!stopState) {
        if (timer0On) {
          timer0Ticks -= clockTicks;
          if (timer0Ticks <= 0) {
            timer0Ticks += (0x10000 - timer0Reload) << timer0ClockReload;
            timerOverflow |= 1;
            soundTimerOverflow(0);
            if (TM0CNT & 0x40) {
              IF |= 0x08;
              UPDATE_REG(0x202, IF);
            }
          }
          TM0D = 0xFFFF - (timer0Ticks >> timer0ClockReload);
          UPDATE_REG(0x100, TM0D);
        }

        if (timer1On) {
          if (TM1CNT & 4) {
            if (timerOverflow & 1) {
            	TM1D++;
              if (TM1D == 0) {
              	TM1D += timer1Reload;
                timerOverflow |= 2;
                soundTimerOverflow(1);
                if (TM1CNT & 0x40) {
                  IF |= 0x10;
                  UPDATE_REG(0x202, IF);
                }
              }
              UPDATE_REG(0x104, TM1D);
            }
          } else {
            timer1Ticks -= clockTicks;
            if (timer1Ticks <= 0) {
              timer1Ticks += (0x10000 - timer1Reload) << timer1ClockReload;
              timerOverflow |= 2;
              soundTimerOverflow(1);
              if (TM1CNT & 0x40) {
                IF |= 0x10;
                UPDATE_REG(0x202, IF);
              }
            }
            TM1D = 0xFFFF - (timer1Ticks >> timer1ClockReload);
            UPDATE_REG(0x104, TM1D);
          }
        }

        if (timer2On) {
          if (TM2CNT & 4) {
            if (timerOverflow & 2) {
            	TM2D++;
              if (TM2D == 0) {
              	TM2D += timer2Reload;
                timerOverflow |= 4;
                if (TM2CNT & 0x40) {
                  IF |= 0x20;
                  UPDATE_REG(0x202, IF);
                }
              }
              UPDATE_REG(0x108, TM2D);
            }
          } else {
            timer2Ticks -= clockTicks;
            if (timer2Ticks <= 0) {
              timer2Ticks += (0x10000 - timer2Reload) << timer2ClockReload;
              timerOverflow |= 4;
              if (TM2CNT & 0x40) {
                IF |= 0x20;
                UPDATE_REG(0x202, IF);
              }
            }
            TM2D = 0xFFFF - (timer2Ticks >> timer2ClockReload);
            UPDATE_REG(0x108, TM2D);
          }
        }

        if (timer3On) {
          if (TM3CNT & 4) {
            if (timerOverflow & 4) {
            	TM3D++;
              if (TM3D == 0) {
              	TM3D += timer3Reload;
                if (TM3CNT & 0x40) {
                  IF |= 0x40;
                  UPDATE_REG(0x202, IF);
                }
              }
              UPDATE_REG(0x10C, TM3D);
            }
          } else {
              timer3Ticks -= clockTicks;
            if (timer3Ticks <= 0) {
              timer3Ticks += (0x10000 - timer3Reload) << timer3ClockReload;
              if (TM3CNT & 0x40) {
                IF |= 0x40;
                UPDATE_REG(0x202, IF);
              }
            }
            TM3D = 0xFFFF - (timer3Ticks >> timer3ClockReload);
            UPDATE_REG(0x10C, TM3D);
          }
        }
      }

      timerOverflow = 0;

#ifdef PROFILING
      profilingTicks -= clockTicks;
      if (profilingTicks <= 0) {
        profilingTicks += profilingTicksReload;
        if (profilSegment) {
	  profile_segment* seg = profilSegment;
	  do {
	    uint16_t* b = (uint16_t*)seg->sbuf;
	    int pc = ((reg[15].I - seg->s_lowpc) * seg->s_scale) / 0x10000;
	    if (pc >= 0 && pc < seg->ssiz) {
            b[pc]++;
	      break;
          }

	    seg = seg->next;
	  } while (seg);
        }
      }
#endif

      //ticks -= clockTicks;

#ifndef NO_LINK
    if (GetLinkMode() != LINK_DISCONNECTED)
		  LinkUpdate(clockTicks);
#endif

      cpuNextEvent = CPUUpdateTicks();

      if (cpuDmaTicksToUpdate > 0) {
        if (cpuDmaTicksToUpdate > cpuNextEvent)
          clockTicks = cpuNextEvent;
        else
          clockTicks = cpuDmaTicksToUpdate;
        cpuDmaTicksToUpdate -= clockTicks;
        if (cpuDmaTicksToUpdate < 0)
        	cpuDmaTicksToUpdate = 0;
        goto updateLoop;
      }

#ifndef NO_LINK
	  // shuffle2: what's the purpose?
    if (GetLinkMode() != LINK_DISCONNECTED || gba_joybus_active)
  	       cpuNextEvent = 1;
#endif

      if (IF && (IME & 1) && armIrqEnable) {
        int res = IF & IE;
        if (stopState)
          res &= 0x3080;
        if (res) {
          if (intState) {
            if (!IRQTicks) {
              CPUInterrupt();
              intState = false;
              holdState = false;
              stopState = false;
              holdType = 0;
            }
          } else {
            if (!holdState) {
            	intState = true;
            	IRQTicks = 7;
#ifdef VBAM_USE_IRQTICKS
              if (cpuNextEvent > IRQTicks)
                cpuNextEvent = IRQTicks;
#else
							if (cpuNextEvent > 7)
								cpuNextEvent = 7;
#endif
            } else {
              CPUInterrupt();
              holdState = false;
              stopState = false;
              holdType = 0;
            }
          }

          // Stops the SWI Ticks emulation if an IRQ is executed
          //(to avoid problems with nested IRQ/SWI)
          if (SWITicks)
            SWITicks = 0;
        }
      }

      if (remainingTicks > 0) {
        if (remainingTicks > cpuNextEvent)
          clockTicks = cpuNextEvent;
        else
          clockTicks = remainingTicks;
        remainingTicks -= clockTicks;
        if (remainingTicks < 0)
          remainingTicks = 0;
        goto updateLoop;
      }

      if (timerOnOffDelay)
          applyTimer(gba, cpu);

      /*if (cpuNextEvent > ticks)
        cpuNextEvent = ticks;*/

      // end loop when a frame is done
      if (cpuBreakLoop)
          break;
    }
  }
#ifndef NO_LINK
    if (GetLinkMode() != LINK_DISCONNECTED)
        CheckLinkConnection();
#endif
}

#if 0
void gbaEmulate(int ticks)
{
    has_frames = false;

    // Read and process inputs
    gbaUpdateJoypads();

    // Runs nth number of ticks till vblank, outputs audio
    // then the video frames.
    // sanity check:
    // wrapped in loop in case frames has not been written yet
    do {
        CPULoop(ticks);
    } while (!has_frames);
}

struct EmulatedSystem GBASystem = {
  // emuMain
	gbaEmulate,
  // emuReset
  CPUReset,
  // emuCleanUp
  CPUCleanUp,
#ifdef __LIBRETRO__
    NULL,           // emuReadBattery
    NULL,           // emuReadState
    CPUReadState,   // emuReadState
    CPUWriteState,  // emuWriteState
    NULL,           // emuReadMemState
    NULL,           // emuWriteMemState
    NULL,           // emuWritePNG
    NULL,           // emuWriteBMP
#else
  // emuReadBattery
  CPUReadBatteryFile,
  // emuWriteBattery
  CPUWriteBatteryFile,
  // emuReadState
  CPUReadState,
  // emuWriteState
  CPUWriteState,
  // emuReadMemState
	CPUReadMemState,
  // emuWriteMemState
  CPUWriteMemState,
  // emuWritePNG
  CPUWritePNGFile,
  // emuWriteBMP
  CPUWriteBMPFile,
#endif
  // emuUpdateCPSR
  CPUUpdateCPSR,
  // emuHasDebugger
  true,
  // emuCount
#ifdef FINAL_VERSION
  300000
#else
  5000
#endif
};
#endif
