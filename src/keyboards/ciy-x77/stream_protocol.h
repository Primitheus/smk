#pragma once
#include <stdint.h>
#include <stdbool.h>

// Single 8-byte control transfer, including report ID. No variable lengths.
// Report 8: [8,'S','L',1,command,R,G,B]. 0 release, 1 fill, 2 begin, 3 commit.
// Report 9: [9,pair_index,R0,G0,B0,R1,G1,B1]. 54 sequential pairs per frame.
static bool stream_valid(const uint8_t *p)
{
    return (p[0] == 8 && p[1] == 'S' && p[2] == 'L' && p[3] == 1 && p[4] <= 3)
        || (p[0] == 9 && p[1] < 54);
}

// Standalone state machine, also exercised by the native host test.
typedef struct {
    uint8_t next_pair;
    uint8_t front;
    bool receiving;
} stream_frame_state;

static bool stream_pair_accept(stream_frame_state *s, uint8_t pair)
{
    if (!s->receiving || pair >= 54 || pair != s->next_pair) {
        s->receiving = false;
        return false;
    }
    s->next_pair++;
    return true;
}

static bool stream_commit(stream_frame_state *s)
{
    bool complete = s->receiving && s->next_pair == 54;
    s->receiving = false;
    if (complete) s->front ^= 1;
    return complete;
}
