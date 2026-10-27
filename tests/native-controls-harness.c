#define ST_CONNECTION_TEST_MODE
#include "../plugin.c"
#include <assert.h>
const char *st_connection_test_url;
struct job {obs_task_t task;void *param;struct job *next;};
static struct job *jobs;
static pthread_mutex_t jobs_lock=PTHREAD_MUTEX_INITIALIZER;
static void enqueue(obs_task_t task,void *param,bool wait){assert(!wait);struct job *j=calloc(1,sizeof(*j));j->task=task;j->param=param;pthread_mutex_lock(&jobs_lock);j->next=jobs;jobs=j;pthread_mutex_unlock(&jobs_lock);}
static void pump(void){pthread_mutex_lock(&jobs_lock);struct job *j=jobs;jobs=NULL;pthread_mutex_unlock(&jobs_lock);while(j){struct job *n=j->next;j->task(j->param);free(j);j=n;}}
static const char *name(void *x){(void)x;return "Local controls test";}
static void *make(obs_data_t *s,obs_source_t *c){(void)s;return c;}
static void drop(void *x){(void)x;}
static bool busy(struct st_filter *f){pthread_mutex_lock(&f->qlock);bool b=f->test_busy;pthread_mutex_unlock(&f->qlock);return b;}
static void probe(struct st_filter *f,const char *path,bool expect_ok){char url[128];snprintf(url,sizeof(url),"http://127.0.0.1:8798/%s",path);st_connection_test_url=url;bool paused=f->user_paused,connected=f->connected;unsigned phase=f->stop_phase;uint64_t reconnects=f->reconnects;int sent=f->audio_chunks_sent;char capture[48];snprintf(capture,sizeof(capture),"%s",f->capture_id);
 assert(st_test_connection_clicked(NULL,NULL,f));uint64_t start=os_gettime_ns();while(busy(f)&&os_gettime_ns()-start<9000000000ULL){pump();os_sleep_ms(10);}assert(!busy(f));pump();assert(f->user_paused==paused && f->connected==connected && f->stop_phase==phase && f->reconnects==reconnects && f->audio_chunks_sent==sent && !strcmp(capture,f->capture_id));assert((strstr(f->test_result,"Server reachable")!=NULL)==expect_ok);assert(!strstr(f->test_result,f->plugin_key));}
int main(void){
 assert(obs_startup("en-US",NULL,NULL));obs_set_ui_task_handler(enqueue);pump();struct obs_audio_info ai={.samples_per_sec=48000,.speakers=SPEAKERS_STEREO};assert(obs_reset_audio(&ai));struct obs_source_info info={.id="controls_qa",.type=OBS_SOURCE_TYPE_INPUT,.output_flags=OBS_SOURCE_AUDIO,.get_name=name,.create=make,.destroy=drop};obs_register_source(&info);obs_source_t *source=obs_source_create_private(info.id,"Local test",NULL);assert(source);
 struct st_filter f={0};f.context=source;pthread_mutex_init(&f.qlock,NULL);pthread_cond_init(&f.test_ready,NULL);snprintf(f.server,sizeof(f.server),"streamtranslate.live");snprintf(f.plugin_key,sizeof(f.plugin_key),"SECRET-NEVER-SENT");snprintf(f.capture_id,sizeof(f.capture_id),"existing-capture");f.connected=true;f.audio_chunks_captured=10;f.audio_chunks_sent=9;
 obs_properties_t *p=st_get_properties(&f);assert(!strcmp(obs_property_description(obs_properties_get(p,"stop_stream")),"Pause sending audio"));assert(!strcmp(obs_property_description(obs_properties_get(p,"test_connection")),"Test connection"));assert(!obs_properties_get(p,"reconnect"));obs_properties_destroy(p);
 uint64_t last=0;st_refresh_live_status(&f,1000000000ULL,&last);assert(strstr(f.status,"captured 10, sent 9"));f.audio_chunks_captured=99;f.audio_chunks_sent=98;st_refresh_live_status(&f,1999999999ULL,&last);assert(strstr(f.status,"captured 10, sent 9"));st_refresh_live_status(&f,2000000000ULL,&last);assert(strstr(f.status,"captured 99, sent 98"));pump();
 probe(&f,"ok",true);assert(st_audio_toggle_clicked(NULL,NULL,&f));assert(f.user_paused);p=st_get_properties(&f);assert(!strcmp(obs_property_description(obs_properties_get(p,"stop_stream")),"Start sending audio"));obs_properties_destroy(p);char paused_status[320];snprintf(paused_status,sizeof(paused_status),"%s",f.status);st_refresh_live_status(&f,3000000000ULL,&last);assert(!strcmp(paused_status,f.status));
 probe(&f,"ok",true);probe(&f,"bad",false);probe(&f,"invalid",false);probe(&f,"redirect",false);probe(&f,"error",false);probe(&f,"large",false);
 char url[320];assert(st_health_url("streamtranslate.live",url,sizeof(url)) && !strcmp(url,"https://streamtranslate.live/health"));assert(st_health_url("host.test:443",url,sizeof(url)));const char *invalid[]={"https://host.test","host.test/path","host.test?key=secret","host.test@evil.test","-host","host:0","host:65536","host:443;foo","host\n.test","host:abc","host:443:444"};for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);i++)assert(!st_health_url(invalid[i],url,sizeof(url)));
 assert(st_audio_toggle_clicked(NULL,NULL,&f));assert(!f.user_paused && !f.stop_phase && !f.stop_deadline_ns && strcmp(f.capture_id,"existing-capture"));
 st_connection_test_url="http://127.0.0.1:8798/slow";st_test_connection_clicked(NULL,NULL,&f);os_sleep_ms(100);uint64_t start=os_gettime_ns();st_connection_test_stop(&f);assert(os_gettime_ns()-start<1500000000ULL);pump();pthread_cond_destroy(&f.test_ready);pthread_mutex_destroy(&f.qlock);obs_source_release(source);obs_shutdown();puts("PASS morphing Pause/Start, independent credential-free probe (active/paused), 1s counters, failure/redirect/size handling, URL validation and bounded cancellation");
}
