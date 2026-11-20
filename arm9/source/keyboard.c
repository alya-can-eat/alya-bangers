// SPDX-License-Identifier: GPL-3.0-or-later

#include <string.h>

#include "keyboard.h"
#include "search.h"
#include "util.h"

static int put(Key *out, int n, int x, int y, int w, int h, int kind, bool fn, uint32_t cp)
{
    out[n] = (Key){ (int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, (uint8_t)kind, fn, cp };
    return n + 1;
}

// A row of characters from a UTF-8 string, pitch apart from x.
static int row(Key *out, int n, const char *s, int x, int y, int w, int h, int pitch, bool shift)
{
    while (*s)
    {
        uint32_t c = utf8_next(&s);
        n = put(out, n, x, y, w, h, K_CHAR, false, shift ? cp_upper(c) : c);
        x += pitch;
    }
    return n;
}

// Bottom row of the Latin and Cyrillic layouts (and of the 123 keys).
static int bottom_row(Key *out, int n, int y)
{
    n = put(out, n, 4, y, 44, 29, K_IM, true, 0);
    n = put(out, n, 50, y, 40, 29, K_NUM, true, 0);
    n = put(out, n, 92, y, 108, 29, K_SPACE, true, ' ');
    return put(out, n, 202, y, 50, 29, K_DONE, true, 0);
}

int kb_layout(int im, bool num, bool qwertz, bool shift, Key *out)
{
    int n = 0;
    if (num)
    {
        n = row(out, n, "1234567890", 4, 0, 23, 29, 25, false);
        n = row(out, n, "-/:;()&@\"", 16, 32, 23, 29, 25, false);
        n = put(out, n, 4, 64, 36, 29, K_CHAR, true, '#');
        n = row(out, n, ".,?!'+*", 42, 64, 23, 29, 25, false);
        n = put(out, n, 217, 64, 37, 29, K_BACK, true, 0);
        return bottom_row(out, n, 96);
    }
    if (im == IM_KANA)
    {
        // the 50-sound table, columns あ..わ from the left; the last column: ん ー ゛ ゜ 小
        static const char *const rows[5] = { "あかさたなはまやらわん", "いきしちにひみ　り　ー", "うくすつぬふむゆる　゛",
                                            "えけせてねへめ　れ　゜", "おこそとのほもよろを小" };
        for (int r = 0; r < 5; r++)
        {
            const char *s = rows[r];
            for (int c = 0; *s; c++)
            {
                uint32_t ch = utf8_next(&s);
                int x = 2 + c * 23, y = r * 21;
                if (ch == 0x3000)           // a gap in the table
                    continue;
                if (ch == 0x309B)
                    n = put(out, n, x, y, 21, 19, K_KANA, true, KANA_DAKU);
                else if (ch == 0x309C)
                    n = put(out, n, x, y, 21, 19, K_KANA, true, KANA_HANDAKU);
                else if (ch == 0x5C0F)
                    n = put(out, n, x, y, 21, 19, K_KANA, true, KANA_SMALL);
                else
                    n = put(out, n, x, y, 21, 19, K_CHAR, c == 10, ch);
            }
        }
        n = put(out, n, 2, 105, 44, 19, K_IM, true, 0);
        n = put(out, n, 48, 105, 40, 19, K_NUM, true, 0);
        n = put(out, n, 90, 105, 100, 19, K_SPACE, true, ' ');
        n = put(out, n, 192, 105, 30, 19, K_BACK, true, 0);
        return put(out, n, 224, 105, 30, 19, K_DONE, true, 0);
    }
    if (im == IM_CYR)
    {
        // ЙЦУКЕН, as on Russian keyboards
        n = row(out, n, "йцукенгшщзхъ", 3, 0, 19, 29, 21, shift);
        n = row(out, n, "фывапролджэ", 13, 32, 19, 29, 21, shift);
        n = put(out, n, 3, 64, 28, 29, K_SHIFT, true, 0);
        n = row(out, n, "ячсмитьбю", 33, 64, 19, 29, 21, shift);
        n = put(out, n, 222, 64, 34, 29, K_BACK, true, 0);
        return bottom_row(out, n, 96);
    }
    // Latin (also for pinyin)
    n = row(out, n, qwertz ? "qwertzuiop" : "qwertyuiop", 4, 0, 23, 29, 25, shift);
    n = row(out, n, "asdfghjkl", 16, 32, 23, 29, 25, shift);
    n = put(out, n, 4, 64, 36, 29, K_SHIFT, true, 0);
    n = row(out, n, qwertz ? "yxcvbnm" : "zxcvbnm", 42, 64, 23, 29, 25, shift);
    n = put(out, n, 217, 64, 37, 29, K_BACK, true, 0);
    return bottom_row(out, n, 96);
}

int kb_key_at(const Key *keys, int n, int x, int y)
{
    // the gaps between keys belong to the nearest key, so no tap is lost
    int best = -1, best_d = 1 << 30;
    for (int i = 0; i < n; i++)
    {
        const Key *k = &keys[i];
        int dx = x < k->x ? k->x - x : (x >= k->x + k->w ? x - (k->x + k->w - 1) : 0);
        int dy = y < k->y ? k->y - y : (y >= k->y + k->h ? y - (k->y + k->h - 1) : 0);
        int d = dx * dx + dy * dy;
        if (d < best_d)
        {
            best = i;
            best_d = d;
        }
    }
    return best_d <= 3 * 3 ? best : -1;
}

const char *kb_im_label(int im)
{
    static const char *const labels[IM_COUNT] = { "ABC", "かな", "拼音", "АБВ" };
    return labels[im >= 0 && im < IM_COUNT ? im : 0];
}

void kb_forms_box(const Key *k, int nforms, int *x, int *y, int *w)
{
    *w = nforms * 25 + 3;
    int x0 = k->x + k->w / 2 - *w / 2;
    if (x0 > 256 - 2 - *w)
        x0 = 256 - 2 - *w;
    if (x0 < 2)
        x0 = 2;
    *x = x0;
    *y = k->y - 34;
}
