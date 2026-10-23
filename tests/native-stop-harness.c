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

struct ui_job {obs_task_t task;void *param;struct ui_job *next;};
static pthread_mutex_t ui_lock=PTHREAD_MUTEX_INITIALIZER;
static struct ui_job *ui_jobs;
static void test_queue_ui(obs_task_t task,void *param,bool wait){assert(!wait);struct ui_job *job=calloc(1,sizeof(*job));job->task=task;job->param=param;pthread_mutex_lock(&ui_lock);struct ui_job **end=&ui_jobs;while(*end)end=&(*end)->next;*end=job;pthread_mutex_unlock(&ui_lock);}
static void test_pump_ui(void){pthread_mutex_lock(&ui_lock);struct ui_job *jobs=ui_jobs;ui_jobs=NULL;pthread_mutex_unlock(&ui_lock);while(jobs){struct ui_job *next=jobs->next;jobs->task(jobs->param);free(jobs);jobs=next;}}


int main(void){
 assert(obs_startup("en-US",NULL,NULL));obs_set_ui_task_handler(test_queue_ui);test_pump_ui();
 struct obs_audio_info ai={.samples_per_sec=48000,.speakers=SPEAKERS_STEREO};assert(obs_reset_audio(&ai));
 struct obs_source_info src={.id="qa_audio",.type=OBS_SOURCE_TYPE_INPUT,.output_flags=OBS_SOURCE_AUDIO,.get_name=name,.create=make_source,.destroy=free_source};obs_register_source(&src);obs_register_source(&st_filter_info);
 obs_source_t *source=obs_source_create_private("qa_audio","QA stop",NULL);obs_set_output_source(0,source);
 obs_data_t *settings=obs_data_create();obs_data_set_string(settings,"server","127.0.0.1");obs_data_set_string(settings,"plugin_key","local-test-only");
 struct st_filter *f=st_create(settings,source);
 float left[480]={0},right[480];double phase=0;uint64_t start=os_gettime_ns();bool stopped1=false,resumed=false,stopped2=false;int sent_at_stop=0;
 for(;;){double age=(os_gettime_ns()-start)/1e9;if(age>12)break;
   if(age>3&&!stopped1){assert(f->connected && f->audio_chunks_sent>30);st_stop_clicked(NULL,NULL,f);sent_at_stop=f->audio_chunks_sent;stopped1=true;}
   if(age>5&&!resumed){assert(!f->connected && st_user_paused(f) && f->reconnects==1 && f->audio_chunks_sent==sent_at_stop);st_reconnect_clicked(NULL,NULL,f);resumed=true;}
   if(age>9&&!stopped2){assert(f->connected && f->reconnects==2 && f->audio_chunks_sent>sent_at_stop+30);st_stop_clicked(NULL,NULL,f);st_stop_clicked(NULL,NULL,f);sent_at_stop=f->audio_chunks_sent;stopped2=true;}
   for(int i=0;i<480;i++){right[i]=0.3f*sin(phase);phase+=2*3.141592653589793*440/48000;}
   struct obs_audio_data a={.data={(uint8_t*)left,(uint8_t*)right},.frames=480,.timestamp=os_gettime_ns()};
   st_filter_audio(f,&a);test_pump_ui();os_sleep_ms(10);
 }
 assert(stopped2 && !f->connected && st_user_paused(f) && f->reconnects==2 && f->audio_chunks_sent==sent_at_stop && f->qcount==0);
 st_destroy(f);test_pump_ui();obs_set_output_source(0,NULL);obs_source_release(source);obs_data_release(settings);obs_shutdown();puts("PASS native Stop sends intent, stops PCM immediately, stays paused, resumes only on Reconnect, and repeated Stop is idempotent");return 0;
}
