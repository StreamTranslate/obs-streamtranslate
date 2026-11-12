#include <obs-module.h>
#include <assert.h>
static void test_queue(enum obs_task_type type, obs_task_t task, void *param, bool wait);
static void test_refresh(obs_source_t *source);
#define obs_queue_task test_queue
#define obs_source_update_properties test_refresh
#define st_copy_plugin_key_to_clipboard test_copy_plugin_key
#include "../plugin.c"
#undef obs_queue_task
#undef obs_source_update_properties
#undef st_copy_plugin_key_to_clipboard
#ifdef __APPLE__
static unsigned clipboard_writes;
bool test_copy_plugin_key(const char *key)
{
    assert(!strcmp(key, "current-synthetic-key"));
    clipboard_writes++;
    return true;
}
#endif
static obs_task_t queued;
static void *queued_data;
static unsigned refreshed;
static struct st_filter f;
static void *test_create(obs_data_t *settings, obs_source_t *context)
{
    (void)settings;
    f.context = context;
    f.source_ref = obs_source_get_weak_source(context);
    return &f;
}
static void test_destroy(void *data) { (void)data; }
static void test_queue(enum obs_task_type type, obs_task_t task, void *param, bool wait)
{ assert(type == OBS_TASK_UI && !wait && !queued); queued=task;queued_data=param; }
static void test_refresh(obs_source_t *source) { assert(source);refreshed++; }
static void drain(void) { obs_task_t task=queued;void *param=queued_data;queued=NULL;queued_data=NULL;task(param); }
int main(void)
{
 assert(obs_startup("en-US",NULL,NULL));
 struct obs_source_info info = {
    .id = "streamtranslate_audio_filter", .type = OBS_SOURCE_TYPE_FILTER,
    .output_flags = OBS_SOURCE_AUDIO, .get_name = st_get_name,
    .create = test_create, .destroy = test_destroy,
 };
 obs_register_source(&info);
 obs_source_t *source=obs_source_create_private(info.id,"status test",NULL);assert(source);
 pthread_mutex_init(&f.qlock,NULL);
 st_set_status(&f,"Starting up");assert(!queued);st_publish_status(&f);assert(queued && refreshed==0);drain();
 st_set_status(&f,"Connected");assert(!queued);st_publish_status(&f);assert(queued && refreshed==1);
 obs_data_t *settings=obs_source_get_settings(source);
#ifdef __APPLE__
 obs_properties_t *properties = st_get_properties(&f);
 obs_property_t *copy_key = obs_properties_get(properties, "copy_plugin_key");
 assert(obs_property_button_clicked(copy_key, source));
 assert(!strcmp(obs_property_description(copy_key), "Enter a plugin key first"));
 assert(!clipboard_writes);
 strcpy(f.plugin_key, "previous-synthetic-key");
 obs_data_set_string(settings, "plugin_key", "current-synthetic-key");
 assert(obs_property_button_clicked(copy_key, source));
 assert(clipboard_writes == 1);
 assert(!strcmp(obs_property_description(copy_key), "Plugin key copied"));
 assert(!strcmp(f.plugin_key, "previous-synthetic-key"));
 obs_properties_destroy(properties);
#endif
 drain();assert(refreshed==2);
 obs_properties_t *status_props=st_get_properties(&f);
 assert(!strcmp(obs_property_description(obs_properties_get(status_props,"status_text")),"Connected"));
 obs_properties_destroy(status_props);
 assert(!obs_data_has_user_value(settings,"status_text"));
 /* Update completion uses the same weak reference as connection status. */
 st_update_refresh_filter(NULL, source, NULL);
 assert(queued && queued_data == f.source_ref);
 drain();assert(refreshed==3);
 st_set_status(&f,"Connected");assert(!queued);
 /* Live packet counts cannot rebuild an open source picker. */
 for(int i=0;i<200;i++){f.audio_chunks_sent++;st_set_status(&f,"Connected");st_publish_status(&f);assert(!queued);}
 assert(strstr(st_setup_close_message("Two speakers cannot start while native CC is enabled or settings are changing"),"native Twitch CC off"));
 assert(!strstr(st_setup_close_message("Guest admission changed; reconnect"),"Mic 1 is not connected"));
 assert(strstr(st_setup_close_message("Streamer is not live"),"reconnects automatically"));
 assert(!st_stop_clicked(NULL,NULL,&f));assert(!queued);st_publish_status(&f);assert(queued);
 obs_data_release(settings);obs_source_release(source);
 obs_wait_for_destroy_queue();drain();assert(refreshed==3);
 /* Status can become dirty after OBS drops its final source reference. */
 st_set_status(&f,"Stopped");st_publish_status(&f);assert(queued);
 drain();assert(refreshed==3);
 obs_weak_source_release(f.source_ref);
 pthread_mutex_destroy(&f.qlock);obs_shutdown();
 puts("status refresh: deferred UI dispatch, live update, deduplication and removal-before-dispatch passed");
}
