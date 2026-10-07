#include <assert.h>
#include "../retry-policy.h"
int main(void) {
 assert(st_close_policy(4402)==ST_RETRY_MANUAL);
 assert(st_close_policy(4400)==ST_RETRY_MANUAL);
 assert(st_close_policy(4005)==ST_RETRY_MANUAL);
 assert(st_close_policy(4008)==ST_RETRY_MANUAL);
 assert(st_close_policy(4403)==ST_RETRY_MANUAL);
 assert(st_close_policy(4003)==ST_RETRY_MANUAL);
 assert(st_close_policy(4004)==ST_RETRY_MANUAL);
 assert(st_close_policy(4001)==ST_RETRY_MANUAL);
 assert(st_close_policy(4009)==ST_RETRY_MANUAL);
 assert(st_close_policy(4010)==ST_RETRY_AUDIO);
 assert(st_close_policy(1006)==ST_RETRY_NETWORK);
 assert(st_close_policy(1012)==ST_RETRY_NETWORK);
 assert(!st_audio_can_retry(0,0,0));
 assert(!st_audio_can_retry(100,100,100));
 assert(st_audio_can_retry(101,100,101));
 assert(!st_audio_can_retry(101,100,1000000102ULL));
 return 0;
}
