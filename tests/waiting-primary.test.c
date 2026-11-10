#include <libwebsockets.h>
static struct lws *test_connect(const struct lws_client_connect_info *);
#define lws_client_connect_via_info test_connect
#include "../plugin.c"
#undef lws_client_connect_via_info
#include <assert.h>
static struct lws *test_connect(const struct lws_client_connect_info *source){struct lws_client_connect_info ci=*source;ci.port=8798;ci.ssl_connection=0;ci.address="127.0.0.1";ci.host="127.0.0.1";return lws_client_connect_via_info(&ci);}
static const char *name(void *x){(void)x;return "Synthetic stereo";}
static void headless_ui(obs_task_t task,void *param,bool wait){(void)wait;task(param);}
int main(void){
 assert(obs_startup("en-US",NULL,NULL));obs_set_ui_task_handler(headless_ui);struct obs_audio_info ai={.samples_per_sec=48000,.speakers=SPEAKERS_STEREO};assert(obs_reset_audio(&ai));
 struct obs_source_info src={.id="qa_audio",.type=OBS_SOURCE_TYPE_INPUT,.output_flags=OBS_SOURCE_AUDIO,.get_name=name};obs_register_source(&src);
 obs_source_t *source=obs_source_create_private("qa_audio","QA recovery",NULL);assert(source);
 obs_data_t *settings=obs_data_create();obs_data_set_string(settings,"server","127.0.0.1");obs_data_set_string(settings,"plugin_key","local-test-only");obs_data_set_int(settings,"mic_mode",1);
 struct st_filter *f=st_create(settings,source);float left[480],right[480];double phase=0;uint64_t start=os_gettime_ns();int step=0;uint64_t attempts=0;
 for(;;){double age=(os_gettime_ns()-start)/1e9;if(age>18)break;
  if(age>4&&step==0){assert(!f->terminal_paused && f->waiting_primary && f->reconnects==2);st_stop_clicked(NULL,NULL,f);attempts=f->reconnects;step++;}
  if(age>10&&step==1){assert(f->user_paused && f->reconnects==attempts);st_reconnect_clicked(NULL,NULL,f);step++;}
  for(int i=0;i<480;i++){left[i]=0.2f*sin(phase);right[i]=0.3f*cos(phase);phase+=2*3.141592653589793*440/48000;}
  struct obs_audio_data a={.data={(uint8_t*)left,(uint8_t*)right},.frames=480,.timestamp=os_gettime_ns()};st_filter_audio(f,&a);os_sleep_ms(10);
 }
 assert(f->terminal_paused && f->reconnects==3 && strstr(f->status,"Plugin key rejected"));st_destroy(f);
 obs_data_set_int(settings,"mic_mode",2);obs_data_set_string(settings,"plugin_key","");f=st_create(settings,source);assert(f->sending_stereo&&!f->resampler_dirty);st_destroy(f);
 obs_source_release(source);obs_data_release(settings);obs_shutdown();puts("PASS Stop cancels waiting-primary retries; rejected credentials remain terminal");
}
