/* Exercises the same real libobs JSON parser and ACK queue as plugin.c. No OBS
 * profile, physical source, websocket, provider or broadcast is created. */
#include "../plugin.c"
#include <assert.h>
int main(void)
{
 struct st_filter f={0};
 obs_startup("en-US",NULL,NULL);
 obs_source_t *dummy=obs_source_create_private("audio_line","CC harness",NULL);
 /* This host can use a minimal registered audio source rather than a device. */
 if(!dummy){static struct obs_source_info info={.id="st_cc_test",.type=OBS_SOURCE_TYPE_INPUT,.output_flags=OBS_SOURCE_AUDIO,.get_name=st_get_name};obs_register_source(&info);dummy=obs_source_create_private("st_cc_test","CC harness",NULL);}
 assert(dummy);f.context=dummy;f.native_twitch_cc=true;
 st_cc_receive(&f,"{\"type\":\"native_cc_reset\",\"v\":1,\"epoch\":\"qa-1\"}");assert(!strcmp(f.cc_state.epoch,"qa-1"));
 char message[2048];uint64_t expires=st_cc_wall_ms()+5000;
 snprintf(message,sizeof(message),"{\"type\":\"native_cc_caption\",\"v\":1,\"epoch\":\"qa-1\",\"seq\":1,\"text\":\"test caption\",\"expiresAt\":%" PRIu64 ",\"duration\":3}",expires);
 st_cc_receive(&f,message);assert(f.cc_ack_count==1);assert(strstr(f.cc_acks[0],"unsupported_output"));
 st_cc_receive(&f,message);assert(f.cc_ack_count==1);
 st_cc_receive(&f,"{\"type\":\"native_cc_reset\",\"v\":\"1\",\"epoch\":\"wrong-type\"}");assert(!strcmp(f.cc_state.epoch,"qa-1"));
 st_cc_receive(&f,"{\"type\":\"native_cc_reset\",\"v\":1,\"epoch\":true}");assert(!strcmp(f.cc_state.epoch,"qa-1"));
 for(unsigned i=2;i<25;i++){
  snprintf(message,sizeof(message),"{\"type\":\"native_cc_caption\",\"v\":1,\"epoch\":\"qa-1\",\"seq\":%u,\"text\":\"日本語\",\"expiresAt\":%" PRIu64 ",\"duration\":3}",i,expires);st_cc_receive(&f,message);
 }
 assert(f.cc_ack_count==8);assert(f.cc_state.sequence==24);
 for(unsigned i=0;i<8;i++)assert(strstr(f.cc_acks[i],"unsupported_text"));
 f.native_twitch_cc=false;st_cc_receive(&f,"{\"type\":\"native_cc_reset\",\"v\":1,\"epoch\":\"no\"}");assert(!strcmp(f.cc_state.epoch,"qa-1"));
 f.native_twitch_cc=true;f.speaker=1;st_cc_receive(&f,"{\"type\":\"native_cc_reset\",\"v\":1,\"epoch\":\"no\"}");assert(!strcmp(f.cc_state.epoch,"qa-1"));
 f.speaker=0;f.user_paused=true;st_cc_receive(&f,"{\"type\":\"native_cc_reset\",\"v\":1,\"epoch\":\"no\"}");assert(!strcmp(f.cc_state.epoch,"qa-1"));
 obs_source_release(dummy);obs_shutdown();puts("native CC real OBS JSON: type checks, disabled/Mic2/paused guards, ACK and bounded queue passed");
 return 0;
}
