// SPDX-License-Identifier: MIT

#include <stdlib.h>
#include <string.h>

#include "queue.h"

#define SEQ_STEP 1024u

static QueueEntry *q;
static int count, cap;
static int pos = -1;
static bool shuffle_on;
static int repeat_mode;
static uint32_t rng = 0x2545F491u;

static uint32_t rand32(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static bool reserve(int n)
{
    if (n <= cap)
        return true;
    int c = cap ? cap : 256;
    while (c < n)
        c *= 2;
    QueueEntry *nq = realloc(q, c * sizeof(QueueEntry));
    if (nq == NULL)
        return false;
    q = nq;
    cap = c;
    return true;
}

// Without shuffle the list order is the original order.
static void renumber(void)
{
    if (shuffle_on)
        return;
    for (int i = 0; i < count; i++)
        q[i].seq = (uint32_t)i * SEQ_STEP;
}

static void shuffle_rest(int from)
{
    for (int i = count - 1; i > from; i--)
    {
        int j = from + (int)(rand32() % (uint32_t)(i - from + 1));
        QueueEntry t = q[i];
        q[i] = q[j];
        q[j] = t;
    }
}

void queue_clear(void)
{
    count = 0;
    pos = -1;
}

bool queue_set(const uint32_t *tracks, int n, int start)
{
    if (!reserve(n))
        return false;
    for (int i = 0; i < n; i++)
        q[i] = (QueueEntry){ tracks[i], (uint32_t)i * SEQ_STEP };
    count = n;
    pos = n ? (start >= 0 && start < n ? start : 0) : -1;
    if (shuffle_on && n)
    {
        // the chosen song first, the rest in random order
        QueueEntry t = q[0];
        q[0] = q[pos];
        q[pos] = t;
        pos = 0;
        shuffle_rest(1);
    }
    return true;
}

static uint32_t max_seq(void)
{
    uint32_t m = 0;
    for (int i = 0; i < count; i++)
        if (q[i].seq > m)
            m = q[i].seq;
    return m;
}

bool queue_insert_next(const uint32_t *tracks, int n)
{
    if (n <= 0 || !reserve(count + n))
        return false;
    int at = pos + 1;
    memmove(&q[at + n], &q[at], (count - at) * sizeof(QueueEntry));
    uint32_t base = pos >= 0 ? q[pos].seq : 0;
    for (int i = 0; i < n; i++)
        q[at + i] = (QueueEntry){ tracks[i], base + 1 + (uint32_t)i };
    count += n;
    if (pos < 0)
        pos = 0;
    renumber();
    return true;
}

bool queue_append(const uint32_t *tracks, int n)
{
    if (n <= 0 || !reserve(count + n))
        return false;
    uint32_t s = count ? max_seq() : 0;
    for (int i = 0; i < n; i++)
        q[count + i] = (QueueEntry){ tracks[i], s + SEQ_STEP * (uint32_t)(i + 1) };
    count += n;
    if (pos < 0)
        pos = 0;
    renumber();
    return true;
}

void queue_remove(int p)
{
    if (p < 0 || p >= count)
        return;
    memmove(&q[p], &q[p + 1], (count - p - 1) * sizeof(QueueEntry));
    count--;
    if (p < pos)
        pos--;
    if (pos >= count)
        pos = count - 1;
    renumber();
}

void queue_move(int from, int to)
{
    if (from < 0 || from >= count || to < 0 || to >= count || from == to)
        return;
    QueueEntry e = q[from];
    if (from < to)
        memmove(&q[from], &q[from + 1], (to - from) * sizeof(QueueEntry));
    else
        memmove(&q[to + 1], &q[to], (from - to) * sizeof(QueueEntry));
    q[to] = e;
    if (pos == from)
        pos = to;
    else if (from < pos && to >= pos)
        pos--;
    else if (from > pos && to <= pos)
        pos++;
    if (shuffle_on)
    {
        // keep the original order roughly in step with what the user wants
        uint32_t lo = to > 0 ? q[to - 1].seq : 0;
        q[to].seq = lo + 1;
    }
    renumber();
}

int queue_count(void) { return count; }
int queue_pos(void) { return pos; }
uint32_t queue_track(int p) { return q[p].track; }
const QueueEntry *queue_entries(void) { return q; }

void queue_jump(int p)
{
    if (p >= 0 && p < count)
        pos = p;
}

bool queue_shuffle(void) { return shuffle_on; }
int queue_repeat(void) { return repeat_mode; }
void queue_set_repeat(int mode) { repeat_mode = mode; }

static int cmp_seq(const void *a, const void *b)
{
    const QueueEntry *x = a, *y = b;
    if (x->seq != y->seq)
        return x->seq < y->seq ? -1 : 1;
    return x->track < y->track ? -1 : x->track > y->track;
}

void queue_set_shuffle(bool on)
{
    if (on == shuffle_on)
        return;
    shuffle_on = on;
    if (count == 0)
        return;
    if (on)
    {
        if (pos > 0)
        {
            QueueEntry t = q[0];
            q[0] = q[pos];
            q[pos] = t;
        }
        pos = pos < 0 ? -1 : 0;
        shuffle_rest(pos < 0 ? 0 : 1);
    }
    else
    {
        QueueEntry cur = pos >= 0 ? q[pos] : (QueueEntry){ 0, 0 };
        qsort(q, count, sizeof(QueueEntry), cmp_seq);
        if (pos >= 0)
            for (int i = 0; i < count; i++)
                if (q[i].seq == cur.seq && q[i].track == cur.track)
                    pos = i;
        renumber();
    }
}

int queue_next_pos(bool automatic)
{
    if (count == 0)
        return -1;
    if (pos < 0)
        return 0;
    if (automatic && repeat_mode == REPEAT_ONE)
        return pos;
    if (pos + 1 < count)
        return pos + 1;
    return repeat_mode != REPEAT_OFF ? 0 : -1;
}

int queue_prev_pos(void)
{
    if (count == 0)
        return -1;
    if (pos > 0)
        return pos - 1;
    return repeat_mode == REPEAT_ALL ? count - 1 : 0;
}

bool queue_restore(const QueueEntry *e, int n, int p, bool shuffle, int repeat)
{
    if (!reserve(n))
        return false;
    memcpy(q, e, n * sizeof(QueueEntry));
    count = n;
    pos = (p >= 0 && p < n) ? p : (n ? 0 : -1);
    shuffle_on = shuffle;
    repeat_mode = repeat;
    return true;
}
