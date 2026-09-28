#include <libwebsockets.h>
static struct lws *test_connect(const struct lws_client_connect_info *);
#define lws_client_connect_via_info test_connect
#include "../plugin.c"
#undef lws_client_connect_via_info
static struct lws *test_connect(const struct lws_client_connect_info *source){struct lws_client_connect_info ci=*source;ci.port=8796;ci.ssl_connection=0;ci.address="127.0.0.1";ci.host="127.0.0.1";return lws_client_connect_via_info(&ci);}
static const char *name(void *x){(void)x;return "Synthetic HDMI audio";}
static void *make_source(obs_data_t *settings,obs_source_t *context){(void)settings;return context;}
static void free_source(void *data){(void)data;}

#include <assert.h>
int main(void){
 assert(obs_startup("en-US",NULL,NULL));
 struct obs_audio_info ai={.samples_per_sec=48000,.speakers=SPEAKERS_STEREO};assert(obs_reset_audio(&ai));
 struct obs_source_info src={.id="qa_audio",.type=OBS_SOURCE_TYPE_INPUT,.output_flags=OBS_SOURCE_AUDIO,.get_name=name,.create=make_source,.destroy=free_source};obs_register_source(&src);obs_register_source(&st_filter_info);
 obs_source_t *source=obs_source_create_private("qa_audio","QA retry",NULL);obs_set_output_source(0,source);
 obs_data_t *settings=obs_data_create();obs_data_set_string(settings,"server","127.0.0.1");obs_data_set_string(settings,"plugin_key","local-test-only");
 struct st_filter *f=st_create(settings,source);
 float left[480]={0},right[480];double phase=0;uint64_t start=os_gettime_ns();bool resume1=false,resume2=false;
 for(;;){double age=(os_gettime_ns()-start)/1e9;if(age>25)break;
   if(age>5&&!resume1){assert(f->terminal_paused && f->reconnects==1);st_reconnect_clicked(NULL,NULL,f);resume1=true;}
   if(age>10&&!resume2){assert(f->terminal_paused && f->reconnects==2);st_reconnect_clicked(NULL,NULL,f);resume2=true;}
   if(age>15&&age<16){assert(f->waiting_audio && f->reconnects==3);}
   for(int i=0;i<480;i++){right[i]=age<17?0:0.3f*sin(phase);phase+=2*3.141592653589793*440/48000;}
   struct obs_audio_data a={.data={(uint8_t*)left,(uint8_t*)right},.frames=480,.timestamp=os_gettime_ns()};
   st_filter_audio(f,&a);os_sleep_ms(10);
 }
 assert(f->reconnects==5 && f->connected && f->audio_chunks_sent>50);
 st_destroy(f);obs_set_output_source(0,NULL);obs_source_release(source);obs_data_release(settings);obs_shutdown();puts("PASS native idle/key pause, explicit resume, no-audio wait, voice wake, service restart and PCM resume");return 0;
}
