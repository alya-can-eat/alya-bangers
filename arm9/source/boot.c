// SPDX-License-Identifier: GPL-3.0-or-later
//
// Starting another .nds through Pico Loader, the way the DSpico's own
// bootloader does it (picoLoaderBootstrap.cpp, LNH team): its ARM9 part goes
// into VRAM A/B (0x06800000), its ARM7 part into VRAM C/D (0x06840000), the
// .nds's path into the ARM7 part's header, then VRAM C/D and the card are
// given to the ARM7 and both CPUs jump into Pico Loader.

#include <nds.h>
#include <nds/arm9/dldi.h>
#include <fat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "boot.h"
#include "ipc.h"
#include "picoLoader7.h"

#define PART_MAX    (256 * 1024)        // VRAM A+B, C+D
#define VRAM_PICO9  ((volatile uint32_t *)0x06800000)
#define VRAM_PICO7  ((volatile uint32_t *)0x06840000)
#define CHUNK       (16 * 1024)

static long part_size(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return -1;
    return (long)st.st_size;
}

bool boot_ready(void)
{
    long s9 = part_size(PICO_LOADER_9), s7 = part_size(PICO_LOADER_7);
    return s9 > 0 && s9 <= PART_MAX && s7 >= (long)sizeof(pload_header7_t) && s7 <= PART_MAX;
}

// A file into VRAM, 32 bits at a time (VRAM takes no 8-bit writes).
static bool load_part(const char *path, volatile uint32_t *dst, uint32_t *buf)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return false;
    size_t n, total = 0;
    while ((n = fread(buf, 1, CHUNK, f)) > 0)
    {
        if (total + n > PART_MAX)
        {
            total = 0;      // too big for its place
            break;
        }
        memset((uint8_t *)buf + n, 0, (4 - n % 4) % 4);
        for (size_t i = 0; i < (n + 3) / 4; i++)
            *dst++ = buf[i];
        total += n;
    }
    bool ok = !ferror(f) && total > 0 && total <= PART_MAX;
    fclose(f);
    return ok;
}

static void copy32(volatile void *dst, const void *src, size_t bytes)
{
    volatile uint32_t *d = dst;
    const uint32_t *s = src;
    for (size_t i = 0; i < bytes / 4; i++)
        d[i] = s[i];
}

bool boot_nds(const char *path)
{
    // the drive: the one in the path, else the one the app runs from
    uint16_t drive = PLOAD_BOOT_DRIVE_DLDI;
    if (strncmp(path, "fat:", 4) == 0)
        path += 4;
    else if (strncmp(path, "sd:", 3) == 0)
    {
        path += 3;
        drive = PLOAD_BOOT_DRIVE_DSI_SD;
    }
    else
    {
        const char *d = fatGetDefaultDrive();
        if (d && strncmp(d, "sd:", 3) == 0)
            drive = PLOAD_BOOT_DRIVE_DSI_SD;
    }
    static pload_params_t params;
    _Static_assert(sizeof(params) % 4 == 0, "copied 32 bits at a time");
    if (path[0] != '/' || strlen(path) >= sizeof(params.romPath) || !boot_ready())
        return false;
    memset(&params, 0, sizeof(params));
    strcpy(params.romPath, path);      // savePath empty: Pico Loader puts the .sav next to it

    uint32_t *buf = malloc(CHUNK + 4);
    if (buf == NULL)
        return false;
    uint32_t banks = vramSetPrimaryBanks(VRAM_A_LCD, VRAM_B_LCD, VRAM_C_LCD, VRAM_D_LCD);
    bool ok = load_part(PICO_LOADER_9, VRAM_PICO9, buf) && load_part(PICO_LOADER_7, VRAM_PICO7, buf);
    free(buf);
    pload_header7_t *h = (pload_header7_t *)VRAM_PICO7;
    if (!ok || h->apiVersion < 1)
    {
        vramRestorePrimaryBanks(banks);     // the app's screens, redrawn on the next frame
        return false;
    }
    h->bootDrive = drive;
    h->dldiDriver = (void *)io_dldi_data;
    copy32(&h->loadParams, &params, sizeof(params));
    if (h->apiVersion >= 2)
        *(volatile uint32_t *)h->v2.launcherPath = 0;      // no way back patched in
    if (h->apiVersion >= 3)
        *(volatile uint32_t *)&h->v3.cheats = 0;           // no cheats

    // the ARM7: it stops the sound and waits for VRAM C/D and the card (main.c)
    StreamCmd c = { 0 };
    c.cmd = STREAM_CMD_BOOT;
    fifoSendDatamsg(FIFO_STREAM_CMD, sizeof(c), (uint8_t *)&c);
    swiWaitForVBlank();
    swiWaitForVBlank();

    REG_IME = 0;
    for (int i = 0; i < 4; i++)
        DMA_CR(i) = 0;
    // both screens show a black backdrop, at full brightness for what comes next
    REG_DISPCNT = 1 << 16;
    REG_DISPCNT_SUB = 1 << 16;
    BG_PALETTE[0] = 0;
    BG_PALETTE_SUB[0] = 0;
    setBrightness(3, 0);

    DC_FlushAll();
    IC_InvalidateAll();
    vramSetBankC(VRAM_C_ARM7_0x06000000);
    vramSetBankD(VRAM_D_ARM7_0x06020000);
    sysSetCardOwner(BUS_OWNER_ARM7);
    ((void (*)(void))0x06800000)();
    for (;;)
        ;
}
