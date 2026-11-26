// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdio.h>
#include <string.h>

#include <nds.h>

#include "hostsim.h"
#include "ipc.h"

SimHardware sim_hw;

static FifoValue32HandlerFunc event_handler;
static void *event_userdata;

bool fifoSetValue32Handler(u32 channel, FifoValue32HandlerFunc h, void *userdata)
{
    event_handler = h;
    event_userdata = userdata;
    return true;
}

bool fifoSendDatamsg(u32 channel, u32 num_bytes, u8 *data)
{
    StreamCmd c;
    if (num_bytes != sizeof(c))
        return false;
    memcpy(&c, data, sizeof(c));
    switch (c.cmd)
    {
        case STREAM_CMD_START:
            sim_hw.running = true;
            sim_hw.left = (const int16_t *)(uintptr_t)c.left;
            sim_hw.right = (const int16_t *)(uintptr_t)c.right;
            sim_hw.ring_bytes = c.ring_bytes;
            sim_hw.block_frames = c.block_frames;
            sim_hw.timer_divider = c.timer_divider;
            sim_hw.generation = c.generation;
            sim_hw.played = 0;
            sim_hw.starts++;
            break;
        case STREAM_CMD_STOP:
            sim_hw.running = false;
            sim_hw.stops++;
            sim_hw.last_fade = c.fade_frames;
            // The real ARM7 answers from its FIFO interrupt; here, right away. The
            // sample it stopped at within the next block: whatever the test set.
            event_handler(STREAM_EVT_MAKE(STREAM_EVT_FRAME, c.generation, sim_hw.stop_frame), event_userdata);
            event_handler(STREAM_EVT_MAKE(STREAM_EVT_STOPPED, c.generation, sim_hw.played),
                          event_userdata);
            break;
        default:
            break;
    }
    return true;
}

bool sim_play_block(int16_t *left, int16_t *right)
{
    if (!sim_hw.running)
        return false;
    uint32_t blocks = sim_hw.ring_bytes / (sim_hw.block_frames * 2);
    uint32_t slot = sim_hw.played % blocks;
    memcpy(left, sim_hw.left + slot * sim_hw.block_frames, sim_hw.block_frames * 2);
    memcpy(right, sim_hw.right + slot * sim_hw.block_frames, sim_hw.block_frames * 2);
    sim_hw.played++;
    event_handler(STREAM_EVT_MAKE(STREAM_EVT_BLOCK, sim_hw.generation, sim_hw.played),
                  event_userdata);
    return true;
}

// ---- video, input, power, storage mocks

static u16 vram[2][256 * 256];

int bgInit(int layer, int type, int size, int map, int tile) { return 0; }
int bgInitSub(int layer, int type, int size, int map, int tile) { return 1; }
u16 *bgGetGfxPtr(int id) { return vram[id & 1]; }
void swiWaitForVBlank(void) {}

static uint32_t keys_down, keys_held;
static int touch_x, touch_y;
void sim_set_keys(uint32_t down, uint32_t held) { keys_down = down; keys_held = held; }
void sim_set_touch(int x, int y) { touch_x = x; touch_y = y; }
void touchRead(touchPosition *t) { t->px = touch_x; t->py = touch_y; }
int sim_backlight = -1;
int sim_brightness;
u32 systemSetBacklightLevel(u32 level) { sim_backlight = level; return level; }
void scanKeys(void) {}
u32 keysDown(void) { return keys_down; }
u32 keysHeld(void) { return keys_held; }
u32 keysDownRepeat(void) { return keys_down; }
void powerOn(u32 bits) {}
void powerOff(u32 bits) {}
bool fatInitDefault(void) { return true; }

void sim_write_ppm(const char *path, const uint16_t *px)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL)
        return;
    fprintf(f, "P6\n256 192\n255\n");
    for (int i = 0; i < 256 * 192; i++)
    {
        uint16_t p = px[i];
        uint8_t rgb[3] = { (uint8_t)((p & 31) * 255 / 31), (uint8_t)(((p >> 5) & 31) * 255 / 31),
                           (uint8_t)(((p >> 10) & 31) * 255 / 31) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}
