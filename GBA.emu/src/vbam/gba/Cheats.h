#ifndef CHEATS_H
#define CHEATS_H

#include <cstdint>

#include "../System.h"

#include <imagine/util/container/ArrayList.hh>
#include <zlib.h>

struct ARM7TDMI;

struct CheatsData {
  int code;
  int size;
  int status;
  bool enabled;
  uint32_t rawaddress;
  uint32_t address;
  uint32_t value;
  uint32_t oldValue;
  char codestring[20];
  char desc[32];
};

namespace IG
{
class ApplicationContext;
}

void cheatsAdd(ARM7TDMI &cpu, const char *codeStr, const char *desc, uint32_t rawaddress, uint32_t address, uint32_t value, int code, int size);
void cheatsAddCheatCode(const char *code, const char *desc);
bool cheatsAddGSACode(ARM7TDMI &cpu, const char *code, const char *desc, bool v3);
bool cheatsAddCBACode(ARM7TDMI &cpu, const char *code, const char *desc);
bool cheatsImportGSACodeFile(ARM7TDMI &cpu, const char *name, int game, bool v3);
void cheatsDelete(ARM7TDMI &cpu, int number, bool restore);
void cheatsDeleteAll(ARM7TDMI &cpu, bool restore);
void cheatsEnable(int number);
void cheatsDisable(ARM7TDMI &cpu, int number);
#ifndef __LIBRETRO__
void cheatsSaveGame(gzFile file);
void cheatsReadGame(gzFile file, int version);
void cheatsReadGameSkip(gzFile file, int version);
void cheatsSaveCheatList(IG::ApplicationContext, const char *file);
bool cheatsLoadCheatList(IG::ApplicationContext, const char *file);
#endif
#ifdef BKPT_SUPPORT
void cheatsWriteMemory(uint32_t address, uint32_t value);
void cheatsWriteHalfWord(uint32_t address, uint16_t value);
void cheatsWriteByte(uint32_t address, uint8_t value);
#endif
int cheatsCheckKeys(ARM7TDMI &cpu, uint32_t keys, uint32_t extended);

extern IG::StaticArrayList<CheatsData, 100> cheatsList;
constexpr size_t CHEATS_LIST_DATA_SIZE = sizeof(CheatsData) * 100;

#endif // CHEATS_H
