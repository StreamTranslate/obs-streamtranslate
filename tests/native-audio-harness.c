#include <libwebsockets.h>
static struct lws *test_connect(const struct lws_client_connect_info *);
#define lws_client_connect_via_info test_connect
#include "../plugin.c"
#undef lws_client_connect_via_info
static struct lws *test_connect(const struct lws_client_connect_info *source){struct lws_client_connect_info ci=*source;ci.port=8796;ci.ssl_connection=0;ci.address="127.0.0.1";ci.host="127.0.0.1";return lws_client_connect_via_info(&ci);}
static const char *name(void *x){(void)x;return "Synthetic HDMI audio";}
static void *make_source(obs_data_t *settings,obs_source_t *context){(void)settings;return context;}
static void free_source(void *data){(void)data;}
int main(void){
 if(!obs_startup("en-US",NULL,NULL))return 2;
 struct obs_audio_info ai={.samples_per_sec=48000,.speakers=SPEAKERS_STEREO};if(!obs_reset_audio(&ai))return 3;
 struct obs_source_info src={.id="qa_audio",.type=OBS_SOURCE_TYPE_INPUT,.output_flags=OBS_SOURCE_AUDIO,.get_name=name,.create=make_source,.destroy=free_source};obs_register_source(&src);obs_register_source(&st_filter_info);
 obs_source_t *source=obs_source_create_private("qa_audio","QA right channel",NULL);
 obs_set_output_source(0,source);
 obs_data_t *settings=obs_data_create();obs_data_set_string(settings,"server","127.0.0.1");obs_data_set_string(settings,"plugin_key","local-test-only");
 obs_source_t *filter=obs_source_create_private("streamtranslate_audio_filter","QA filter",settings);obs_source_filter_add(source,filter);
 obs_source_t *silent=obs_source_create_private("qa_audio","QA silent newcomer",NULL),*silent_filter=NULL;
 float left[480]={0},right[480];double phase=0;uint64_t start=os_gettime_ns();bool didMute=false,didUnmute=false;
 for(int frame=0;frame<8500;frame++){
   double age=(os_gettime_ns()-start)/1e9;
   if(age>8 && !silent_filter){silent_filter=obs_source_create_private("streamtranslate_audio_filter","QA silent filter",settings);obs_source_filter_add(silent,silent_filter);}
   if(age>15&&!didMute){obs_source_set_muted(source,true);didMute=true;blog(LOG_INFO,"QA_STAGE mute");}
   if(age>20&&!didUnmute){obs_source_set_muted(source,false);didUnmute=true;blog(LOG_INFO,"QA_STAGE unmute");}
   if(age>=30&&age<58){os_sleep_ms(10);continue;}
   for(int i=0;i<480;i++){right[i]=0.3f*sin(phase);phase+=2*3.141592653589793*440/48000;}
   struct obs_source_audio a={.data={(uint8_t*)left,(uint8_t*)right},.frames=480,.speakers=SPEAKERS_STEREO,.format=AUDIO_FORMAT_FLOAT_PLANAR,.samples_per_sec=48000,.timestamp=os_gettime_ns()};
   obs_source_output_audio(source,&a);os_sleep_ms(10);
 }
 if(silent_filter){obs_source_filter_remove(silent,silent_filter);obs_source_release(silent_filter);}obs_source_release(silent);
 obs_source_filter_remove(source,filter);obs_source_release(filter);obs_set_output_source(0,NULL);obs_source_release(source);obs_data_release(settings);obs_shutdown();return 0;
}
