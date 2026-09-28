#ifndef ST_RETRY_POLICY_H
#define ST_RETRY_POLICY_H
#include <stdint.h>
enum st_retry_policy { ST_RETRY_NETWORK, ST_RETRY_MANUAL, ST_RETRY_AUDIO };
static inline enum st_retry_policy st_close_policy(unsigned code)
{
    switch(code) {
    case 4001: case 4003: case 4004: case 4008: case 4009: case 4403:
        return ST_RETRY_MANUAL;
    case 4010: return ST_RETRY_AUDIO;
    default: return ST_RETRY_NETWORK;
    }
}
static inline int st_audio_can_retry(uint64_t audio, uint64_t stopped, uint64_t now)
{
    return audio > stopped && now >= audio && now-audio < 1000000000ULL;
}
#endif
