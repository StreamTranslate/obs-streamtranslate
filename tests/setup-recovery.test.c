#include <libwebsockets.h>
static struct lws *test_connect(const struct lws_client_connect_info *);
#define lws_client_connect_via_info test_connect
#include "../plugin.c"
#undef lws_client_connect_via_info
#include <assert.h>
static struct lws *test_connect(const struct lws_client_connect_info *source){struct lws_client_connect_info ci=*source;ci.port=8797;ci.ssl_connection=0;ci.address="127.0.0.1";ci.host="127.0.0.1";return lws_client_connect_via_info(&ci);}
static const char *name(void *x){(void)x;return "Synthetic stereo";}
static void headless_ui(obs_task_t task,void *param,bool wait){(void)wait;task(param);}
int main(void){
 assert(obs_startup("en-US",NULL,NULL));obs_set_ui_task_handler(headless_ui);struct obs_audio_info ai={.samples_per_sec=48000,.speakers=SPEAKERS_STEREO};assert(obs_reset_audio(&ai));
 struct obs_source_info src={.id="qa_audio",.type=OBS_SOURCE_TYPE_INPUT,.output_flags=OBS_SOURCE_AUDIO,.get_name=name};obs_register_source(&src);
 obs_source_t *source=obs_source_create_private("qa_audio","QA recovery",NULL);assert(source);
 obs_data_t *settings=obs_data_create();obs_data_set_string(settings,"server","127.0.0.1");obs_data_set_string(settings,"plugin_key","local-test-only");obs_data_set_int(settings,"mic_mode",1);
 struct st_filter *f=st_create(settings,source);float left[480],right[480];double phase=0;uint64_t start=os_gettime_ns();int step=0, sent_at_stop=0;
 for(;;){double age=(os_gettime_ns()-start)/1e9;if(age>30)break;
  if(age>4&&step==0){assert(!f->terminal_paused&&f->reconnects==2&&f->connected);obs_data_set_int(settings,"mic_mode",2);st_update(f,settings);step++;}
  if(age>8&&step==1){assert(f->connected&&f->sending_stereo);st_reconnect_clicked(NULL,NULL,f);step++;}
  if(age>12&&step==2){
   assert(!f->connected&&st_user_paused(f)&&!f->conflict_paused&&f->sending_stereo&&f->reconnects==4);
   assert(strstr(f->status,"Paused - not sending audio"));
   sent_at_stop=f->audio_chunks_sent;
   obs_data_set_int(settings,"mic_mode",0);st_update(f,settings);step++;
  }
  if(age>16&&step==3){
   assert(!f->connected&&st_user_paused(f)&&f->reconnects==4&&f->audio_chunks_sent==sent_at_stop);
   st_audio_toggle_clicked(NULL,NULL,f);step++;
  }
  if(age>23&&step==4){
   assert(f->connected&&!f->sending_stereo&&f->reconnects==6);
   obs_data_set_int(settings,"mic_mode",2);st_update(f,settings);step++;
  }
  if(age>27&&step==5){
   assert(!f->connected&&!st_user_paused(f)&&f->conflict_paused&&f->reconnects==7);
   assert(strstr(f->status,"Another filter already sends this mic"));
   st_audio_toggle_clicked(NULL,NULL,f);step++;
  }
  for(int i=0;i<480;i++){left[i]=0.2f*sin(phase);right[i]=0.3f*cos(phase);phase+=2*3.141592653589793*440/48000;}
  struct obs_audio_data a={.data={(uint8_t*)left,(uint8_t*)right},.frames=480,.timestamp=os_gettime_ns()};st_filter_audio(f,&a);os_sleep_ms(10);
 }
 assert(f->connected&&f->sending_stereo&&f->reconnects==8&&!st_user_paused(f)&&!f->terminal_paused);
 obs_source_set_enabled(source,false);
 struct obs_audio_data muted={.data={(uint8_t*)left,(uint8_t*)right},.frames=480,.timestamp=os_gettime_ns()};
 st_filter_audio(f,&muted);assert(!f->audio_started && strstr(f->status,"Muted in OBS"));
 obs_source_set_enabled(source,true);
 for(int i=0;i<50;i++){st_filter_audio(f,&muted);os_sleep_ms(10);}
 assert(f->audio_started && (strstr(f->status,"Connected")||strstr(f->status,"packets")));st_destroy(f);
 obs_data_set_int(settings,"mic_mode",2);obs_data_set_string(settings,"plugin_key","");f=st_create(settings,source);assert(f->sending_stereo&&!f->resampler_dirty);st_destroy(f);
 obs_source_release(source);obs_data_release(settings);obs_shutdown();puts("PASS missing-primary automatic recovery, mode recovery, stereo reconnect, automatic recovery, owner Stop, separate filter conflict, and channel selection");
}
