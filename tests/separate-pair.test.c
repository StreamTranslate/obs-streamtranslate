#include <libwebsockets.h>
static struct lws *test_connect(const struct lws_client_connect_info *);
#define lws_client_connect_via_info test_connect
#include "../plugin.c"
#undef lws_client_connect_via_info
#include <assert.h>
static struct lws *test_connect(const struct lws_client_connect_info *source){struct lws_client_connect_info ci=*source;ci.port=8798;ci.ssl_connection=0;ci.address="127.0.0.1";ci.host="127.0.0.1";return lws_client_connect_via_info(&ci);}
static pthread_mutex_t ui_lock=PTHREAD_MUTEX_INITIALIZER;
static struct{obs_task_t task;void *param;}ui_jobs[128];static unsigned ui_count;
/* Real OBS dispatches worker property refreshes on its UI thread. Running a
 * queued refresh inline would release its last source reference on the worker
 * itself and make this harness join that same worker during destruction. */
static void headless_ui(obs_task_t task,void *param,bool wait){assert(!wait);pthread_mutex_lock(&ui_lock);assert(ui_count<128);ui_jobs[ui_count].task=task;ui_jobs[ui_count++].param=param;pthread_mutex_unlock(&ui_lock);}
static void drain_ui(void){for(;;){pthread_mutex_lock(&ui_lock);if(!ui_count){pthread_mutex_unlock(&ui_lock);break;}obs_task_t task=ui_jobs[--ui_count].task;void *param=ui_jobs[ui_count].param;pthread_mutex_unlock(&ui_lock);task(param);}}
int main(void){
 alarm(15); /* Fail a stuck cleanup instead of leaving a hung QA process. */
 assert(obs_startup("en-US",NULL,NULL));obs_set_ui_task_handler(headless_ui);
 struct obs_audio_info ai={.samples_per_sec=48000,.speakers=SPEAKERS_STEREO};assert(obs_reset_audio(&ai));
 struct obs_source_info input={.id="qa_pair_input",.type=OBS_SOURCE_TYPE_INPUT,.output_flags=OBS_SOURCE_AUDIO,.get_name=st_get_name};obs_register_source(&input);obs_register_source(&st_filter_info);
 obs_source_t *sources[2],*filters[2];struct st_filter *f[2];
 for(int i=0;i<2;i++){
  sources[i]=obs_source_create_private("qa_pair_input",i?"QA MacBook source":"QA DJI source",NULL);assert(sources[i]);
  obs_data_t *s=obs_data_create();obs_data_set_string(s,"server","127.0.0.1");obs_data_set_string(s,"plugin_key","local-test-only");obs_data_set_int(s,"speaker_count",2);obs_data_set_int(s,"mic_wiring",1);obs_data_set_int(s,"source_role",i);
  filters[i]=obs_source_create_private("streamtranslate_audio_filter",i?"QA Mic 2 filter":"QA Mic 1 filter",s);assert(filters[i]);obs_source_filter_add(sources[i],filters[i]);obs_data_release(s);
  f[i]=obs_obj_get_data(filters[i]);assert(f[i]&&f[i]->speaker==i&&!f[i]->sending_stereo);
 }
 float left[480],right[480];uint64_t start=os_gettime_ns();
 while(os_gettime_ns()-start<7000000000ULL){
  for(int mic=0;mic<2;mic++){
   for(int n=0;n<480;n++)left[n]=right[n]=(mic?0.1f:0.3f)*sin(2*3.141592653589793*(mic?700:440)*n/48000);
   struct obs_audio_data a={.data={(uint8_t*)left,(uint8_t*)right},.frames=480,.timestamp=os_gettime_ns()};st_filter_audio(f[mic],&a);
  }
  drain_ui();os_sleep_ms(10);
 }
 uint64_t cleanup_started=os_gettime_ns();
 for(int i=0;i<2;i++){assert(f[i]->connected&&f[i]->audio_chunks_sent>100);assert(strstr(f[i]->status,"Connected — sending audio"));obs_source_filter_remove(sources[i],filters[i]);obs_source_release(filters[i]);obs_source_release(sources[i]);}
 drain_ui();obs_wait_for_destroy_queue();drain_ui();obs_shutdown();assert(os_gettime_ns()-cleanup_started<3000000000ULL);alarm(0);puts("PASS two real libobs filter instances send independent audio and source names concurrently; clean shutdown under 3 seconds");
}
