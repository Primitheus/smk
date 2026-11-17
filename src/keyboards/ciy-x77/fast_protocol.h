#pragma once
#include <stdint.h>
#include <stdbool.h>

// 64 bytes: [10,'F',1,sequence,chunk,count,flags,57 bytes of RGB].
// Six chunks: 19+19+19+19+19+13 cells. flags=1/count=0 releases lighting.
typedef struct {
    uint8_t sequence;
    uint8_t next;
    bool receiving;
} fast_frame_state;

// 0 reject; 1 accepted chunk; 2 final chunk; 3 release.
static uint8_t fast_accept(fast_frame_state *s, const uint8_t *p)
{
    if (p[0] != 10 || p[1] != 'F' || p[2] != 1) goto reject;
    if (p[6] == 1 && p[4] == 0 && p[5] == 0) {
        s->receiving = false;
        return 3;
    }
    if (p[6] != 0 || p[4] >= 6 || p[5] != (p[4] == 5 ? 13 : 19)) goto reject;
    if (p[4] == 0) {
        s->sequence = p[3];
        s->next = 0;
        s->receiving = true;
    }
    if (!s->receiving || p[3] != s->sequence || p[4] != s->next) goto reject;
    s->next++;
    if (s->next == 6) {
        s->receiving = false;
        return 2;
    }
    return 1;
reject:
    s->receiving = false;
    return 0;
}
