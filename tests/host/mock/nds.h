// Host-test mock of libnds: just enough for the ARM9 sources to run on a PC.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "nds/fifocommon.h"

#define BIT(n) (1u << (n))
enum {
    KEY_A = BIT(0), KEY_B = BIT(1), KEY_SELECT = BIT(2), KEY_START = BIT(3),
    KEY_RIGHT = BIT(4), KEY_LEFT = BIT(5), KEY_UP = BIT(6), KEY_DOWN = BIT(7),
    KEY_R = BIT(8), KEY_L = BIT(9), KEY_X = BIT(10), KEY_Y = BIT(11),
    KEY_TOUCH = BIT(12), KEY_LID = BIT(13),
};
enum { PM_BACKLIGHT_BOTTOM = BIT(2), PM_BACKLIGHT_TOP = BIT(3) };
enum { MODE_5_2D = 5, VRAM_A_MAIN_BG = 1, VRAM_C_SUB_BG = 4, BgType_Bmp16 = 7, BgSize_B16_256x256 = 1 };

static inline void DC_FlushRange(const void *p, u32 n) { (void)p; (void)n; }
static inline void dmaCopy(const void *s, void *d, u32 n) { memcpy(d, s, n); }
static inline void swiDelay(u32 n) { (void)n; }
void swiWaitForVBlank(void);
static inline void videoSetMode(u32 m) { (void)m; }
static inline void videoSetModeSub(u32 m) { (void)m; }
static inline void vramSetBankA(int m) { (void)m; }
static inline void vramSetBankC(int m) { (void)m; }
static inline void lcdMainOnTop(void) {}
int bgInit(int layer, int type, int size, int map, int tile);
int bgInitSub(int layer, int type, int size, int map, int tile);
u16 *bgGetGfxPtr(int id);
void scanKeys(void);
u32 keysDown(void); u32 keysHeld(void); u32 keysDownRepeat(void);
static inline void keysSetRepeat(u8 a, u8 b) { (void)a; (void)b; }
void powerOn(u32 bits); void powerOff(u32 bits);
static inline void disableSleep(void) {}
static inline void defaultExceptionHandler(void) {}
typedef struct touchPosition { u16 rawx, rawy, px, py, z1, z2; } touchPosition;
void touchRead(touchPosition *t);
u32 systemSetBacklightLevel(u32 level);
