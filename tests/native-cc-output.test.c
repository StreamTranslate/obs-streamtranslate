/* Link-time host doubles prove the output dispatch and balanced references. The
 * actual plugin implementation is compiled below; no stream is started. */
#define obs_output_active cc_host_active
#define obs_output_get_service cc_host_service
#define obs_service_get_settings cc_host_settings
#define obs_output_get_video_encoder cc_host_encoder
#define obs_encoder_get_codec cc_host_codec
#define obs_output_release cc_host_release
#include "../plugin.c"
#include <assert.h>
static bool active=true,has_output=true,twitch=true,h264=true,has_codec=true;
static unsigned released,submitted;
static obs_output_t *output=(obs_output_t *)(uintptr_t)0x1234;
obs_output_t *obs_frontend_get_streaming_output(void){return has_output?output:NULL;}
void obs_output_output_caption_text2(obs_output_t *o,const char *text,double duration){assert(o==output);assert(!strcmp(text,"Hello"));assert(duration==0);submitted++;}
bool cc_host_active(const obs_output_t *o){assert(o==output);return active;}
obs_service_t *cc_host_service(const obs_output_t *o){assert(o==output);return (obs_service_t *)(uintptr_t)0x5678;}
obs_data_t *cc_host_settings(const obs_service_t *s){(void)s;obs_data_t *d=obs_data_create();obs_data_set_string(d,"service",twitch?"Twitch":"YouTube - RTMPS");return d;}
obs_encoder_t *cc_host_encoder(const obs_output_t *o){assert(o==output);return (obs_encoder_t *)(uintptr_t)0x9876;}
const char *cc_host_codec(const obs_encoder_t *e){(void)e;return has_codec?(h264?"h264":"av1"):NULL;}
void cc_host_release(obs_output_t *o){assert(o==output);released++;}
int main(void){
 assert(!strcmp(st_cc_submit("Hello",3),"submitted"));assert(submitted==1&&released==1);
 active=false;assert(!strcmp(st_cc_submit("Hello",3),"not_streaming"));assert(submitted==1&&released==2);
 active=true;twitch=false;assert(!strcmp(st_cc_submit("Hello",3),"unsupported_output"));assert(submitted==1&&released==3);
 twitch=true;h264=false;assert(!strcmp(st_cc_submit("Hello",3),"unsupported_output"));assert(submitted==1&&released==4);
 has_codec=false;assert(!strcmp(st_cc_submit("Hello",3),"unsupported_output"));assert(submitted==1&&released==5);
 has_output=false;assert(!strcmp(st_cc_submit("Hello",3),"not_streaming"));assert(submitted==1&&released==5);
 puts("native CC output: Twitch/H264 only, inactive/missing output and reference balance passed");return 0;
}
