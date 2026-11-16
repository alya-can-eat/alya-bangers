// Host-test mock of the libnds FIFO API (only what dsplayer uses).
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef int32_t s32;
enum { FIFO_USER_01 = 8, FIFO_USER_02 = 9 };
typedef void (*FifoValue32HandlerFunc)(u32 value32, void *userdata);
bool fifoSetValue32Handler(u32 channel, FifoValue32HandlerFunc h, void *userdata);
bool fifoSendDatamsg(u32 channel, u32 num_bytes, u8 *data);
