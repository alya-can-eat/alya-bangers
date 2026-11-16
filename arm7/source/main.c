// SPDX-License-Identifier: MIT
//
// ARM7: input, power management, and the two sound channels that stream the
// ring buffers the ARM9 fills.
//
// Timing: sound channel timers count at BUS_CLOCK / 2 = 16756991 Hz, so a
// divider of 512 plays 32728.498 samples per second, which is exactly the
// mixer's output rate. A normal timer with the /1024 prescaler counts at
// BUS_CLOCK / 1024, the very same rate from the same crystal, so it counts
// played samples without drift. It interrupts once per block; each interrupt
// tells the ARM9 that one more ring slot is free.

#include <nds.h>

#include "ipc.h"

#define CH_LEFT         0
#define CH_RIGHT        1
#define STREAM_TIMER    1   // timer 0 is used by Maxmod, 2/3 by Wi-Fi/RTC (unused here)

static volatile uint32_t blocks_played;
static volatile uint8_t generation;
static volatile bool exit_loop;

static void power_button_callback(void)
{
    exit_loop = true;
}

static void stream_timer_irq(void)
{
    blocks_played++;
    fifoSendValue32(FIFO_STREAM_EVENT,
                    STREAM_EVT_MAKE(STREAM_EVT_BLOCK, generation, blocks_played));
}

static void stream_stop(void)
{
    int ime = enterCriticalSection();
    TIMER_CR(STREAM_TIMER) = 0;
    REG_SOUNDXCNT(CH_LEFT) = 0;
    REG_SOUNDXCNT(CH_RIGHT) = 0;
    leaveCriticalSection(ime);
}

static void stream_start(const StreamCmd *c)
{
    stream_stop();

    blocks_played = 0;
    generation = c->generation;

    uint32_t words = c->ring_bytes / 4;
    uint16_t tmr = (uint16_t)(0x10000 - c->timer_divider);

    REG_SOUNDXSAD(CH_LEFT) = c->left;
    REG_SOUNDXTMR(CH_LEFT) = tmr;
    REG_SOUNDXPNT(CH_LEFT) = 0;
    REG_SOUNDXLEN(CH_LEFT) = words;

    REG_SOUNDXSAD(CH_RIGHT) = c->right;
    REG_SOUNDXTMR(CH_RIGHT) = tmr;
    REG_SOUNDXPNT(CH_RIGHT) = 0;
    REG_SOUNDXLEN(CH_RIGHT) = words;

    TIMER_DATA(STREAM_TIMER) = (uint16_t)(0x10000 - c->block_frames);

    uint32_t cnt = SOUNDXCNT_VOL_MUL(c->volume) | SOUNDXCNT_VOL_DIV_1 | SOUNDXCNT_REPEAT
                 | SOUNDXCNT_FORMAT_16BIT | SOUNDXCNT_ENABLE;

    // Start both channels and the counting timer back to back, so they stay
    // within a few CPU cycles of each other (one sample is 1024 cycles).
    int ime = enterCriticalSection();
    REG_SOUNDXCNT(CH_LEFT) = cnt | SOUNDXCNT_PAN(0);
    REG_SOUNDXCNT(CH_RIGHT) = cnt | SOUNDXCNT_PAN(127);
    TIMER_CR(STREAM_TIMER) = TIMER_ENABLE | TIMER_IRQ_REQ | TIMER_DIV_1024;
    leaveCriticalSection(ime);
}

static void stream_cmd_handler(int bytes, void *userdata)
{
    StreamCmd c;
    if (bytes != (int)sizeof(c))
    {
        // Unknown message size: drain it and ignore it.
        uint8_t junk[64];
        fifoGetDatamsg(FIFO_STREAM_CMD, sizeof(junk), junk);
        return;
    }
    fifoGetDatamsg(FIFO_STREAM_CMD, sizeof(c), (uint8_t *)&c);

    switch (c.cmd)
    {
        case STREAM_CMD_START:
            stream_start(&c);
            break;
        case STREAM_CMD_STOP:
            stream_stop();
            fifoSendValue32(FIFO_STREAM_EVENT,
                            STREAM_EVT_MAKE(STREAM_EVT_STOPPED, c.generation, blocks_played));
            break;
        case STREAM_CMD_VOLUME:
            REG_SOUNDXVOL(CH_LEFT) = c.volume;
            REG_SOUNDXVOL(CH_RIGHT) = c.volume;
            break;
        default:
            break;
    }
}

static void vblank_handler(void)
{
    inputGetAndSend();
}

int main(int argc, char *argv[])
{
    enableSound();
    readUserSettings();
    ledBlink(LED_ALWAYS_ON);
    touchInit();

    irqInit();
    fifoInit();
    installSystemFIFO(); // power, sleep, storage

    // A music player keeps playing with the lid closed: never sleep on lid close.
    inputSetLidSleepDuration(0);

    REG_SOUNDCNT = SOUND_ENABLE | SOUND_VOL(127);

    fifoSetDatamsgHandler(FIFO_STREAM_CMD, stream_cmd_handler, NULL);
    irqSet(IRQ_TIMER(STREAM_TIMER), stream_timer_irq);
    irqEnable(IRQ_TIMER(STREAM_TIMER));

    setPowerButtonCB(power_button_callback);

    irqSet(IRQ_VBLANK, vblank_handler);
    irqEnable(IRQ_VBLANK);

    while (!exit_loop)
        swiWaitForVBlank();

    stream_stop();
    return 0;
}
