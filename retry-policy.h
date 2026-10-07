#ifndef ST_RETRY_POLICY_H
#define ST_RETRY_POLICY_H
#include <stdint.h>
enum st_retry_policy { ST_RETRY_NETWORK, ST_RETRY_MANUAL, ST_RETRY_AUDIO };
static inline enum st_retry_policy st_close_policy(unsigned code)
{
    switch(code) {
    case 4001: case 4003: case 4004: case 4005: case 4008: case 4009: case 4400: case 4402: case 4403:
        return ST_RETRY_MANUAL;
    case 4010: return ST_RETRY_AUDIO;
    default: return ST_RETRY_NETWORK;
    }
}
static inline const char *st_close_message(unsigned code)
{
    switch (code) {
    case 4003: return "Account access blocked (trial/activation). Check your plan and current key on Control, then Reconnect.";
    case 4004: case 4402: return "Plan hours exhausted. Check your plan on Control before reconnecting.";
    case 4005: return "This capture mode requires a different plan. Check Control.";
    case 4008: return "Stopped after idle timeout. Press Reconnect to start again.";
    case 4403: return "Plugin key rejected. Copy your current key from Control, then Reconnect.";
    case 4400: return "Server configuration rejected. Check the Server field and contact support.";
    default: return "Session stopped by server. Check Control, then Reconnect.";
    }
}
static inline int st_audio_can_retry(uint64_t audio, uint64_t stopped, uint64_t now)
{
    return audio > stopped && now >= audio && now-audio < 1000000000ULL;
}
#endif
