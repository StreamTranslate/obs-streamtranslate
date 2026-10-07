#include "../plugin.c"
#include <assert.h>
static pthread_mutex_t tasks_lock=PTHREAD_MUTEX_INITIALIZER;
static obs_task_t queued_task;
static void *queued_param;
static unsigned refreshes;
static pthread_t ui_thread;
static void enqueue_ui(obs_task_t task,void *param,bool wait){assert(!wait);pthread_mutex_lock(&tasks_lock);assert(!queued_task);queued_task=task;queued_param=param;pthread_mutex_unlock(&tasks_lock);}
static void pump_ui(void){pthread_mutex_lock(&tasks_lock);obs_task_t task=queued_task;void *param=queued_param;queued_task=NULL;queued_param=NULL;pthread_mutex_unlock(&tasks_lock);if(task)task(param);}
static void refreshed(void *unused,calldata_t *data){(void)unused;(void)data;assert(pthread_equal(ui_thread,pthread_self()));refreshes++;}
static const char *source_name(void *x){(void)x;return "Diagnostic test source";}
static void *source_create(obs_data_t *s,obs_source_t *c){(void)s;return c;}
static void source_destroy(void *data){(void)data;}
static void *network_status(void *data){struct st_filter *f=data;f->connected=true;st_set_status(f,"Connected to streamtranslate.live - streaming audio");return NULL;}
int main(void){
 assert(obs_startup("en-US",NULL,NULL));struct obs_audio_info ai={.samples_per_sec=48000,.speakers=SPEAKERS_STEREO};assert(obs_reset_audio(&ai));ui_thread=pthread_self();obs_set_ui_task_handler(enqueue_ui);pump_ui();
 struct obs_source_info info={.id="diagnostic_test_source",.type=OBS_SOURCE_TYPE_INPUT,.output_flags=OBS_SOURCE_AUDIO,.get_name=source_name,.create=source_create,.destroy=source_destroy};obs_register_source(&info);
 obs_source_t *source=obs_source_create_private(info.id,"Diagnostics",NULL);assert(source);
 signal_handler_connect(obs_source_get_signal_handler(source),"update_properties",refreshed,NULL);
 struct st_filter f={0};f.context=source;pthread_mutex_init(&f.qlock,NULL);snprintf(f.plugin_key,sizeof(f.plugin_key),"SECRET-MUST-NOT-APPEAR");
 pthread_t network;pthread_create(&network,NULL,network_status,&f);pthread_join(network,NULL);
 assert(refreshes==0 && queued_task);pump_ui();assert(refreshes==1);
 obs_properties_t *props=st_get_properties(&f);assert(!obs_property_visible(obs_properties_get(props,"diagnostics_text")));obs_properties_destroy(props);
 assert(st_diagnostics_clicked(NULL,NULL,&f));props=st_get_properties(&f);assert(obs_property_visible(obs_properties_get(props,"diagnostics_text")));
 obs_data_t *settings=obs_source_get_settings(source);const char *status=obs_data_get_string(settings,"status_text");const char *diagnostic=obs_data_get_string(settings,"diagnostics_text");assert(strstr(status,"Connected to"));assert(strstr(diagnostic,"Connection: Connected"));assert(!strstr(diagnostic,f.plugin_key));obs_data_release(settings);obs_properties_destroy(props);
 f.connected=false;f.terminal_paused=true;f.last_close_code=4003;char text[1600];st_diagnostic_text(&f,text,sizeof(text));assert(strstr(text,"Blocked by server") && strstr(text,"4003"));assert(strstr(st_close_message(4003),"Account access blocked"));assert(strstr(st_close_message(4402),"hours exhausted"));
 assert(st_diagnostics_clicked(NULL,NULL,&f) && !f.show_diagnostics);
 obs_source_release(source);pthread_mutex_destroy(&f.qlock);obs_shutdown();puts("PASS deferred UI refresh, visible diagnostics, rejection explanation, secret redaction, toggle");return 0;
}
