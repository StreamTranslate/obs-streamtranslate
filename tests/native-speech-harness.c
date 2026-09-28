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

int main(int argc,char **argv){
 assert(argc==2);FILE *input=fopen(argv[1],"rb");assert(input);fseek(input,0,SEEK_END);long size=ftell(input);rewind(input);int16_t *pcm=malloc(size);assert(fread(pcm,1,size,input)==(size_t)size);fclose(input);
 assert(obs_startup("en-US",NULL,NULL));struct obs_audio_info ai={.samples_per_sec=48000,.speakers=SPEAKERS_STEREO};assert(obs_reset_audio(&ai));
 struct obs_source_info src={.id="qa_audio",.type=OBS_SOURCE_TYPE_INPUT,.output_flags=OBS_SOURCE_AUDIO,.get_name=name,.create=make_source,.destroy=free_source};obs_register_source(&src);
 obs_source_t *source=obs_source_create_private("qa_audio","QA speech",NULL);obs_set_output_source(0,source);
 obs_data_t *settings=obs_data_create();obs_data_set_string(settings,"server","127.0.0.1");obs_data_set_string(settings,"plugin_key","local-test-only");struct st_filter *f=st_create(settings,source);
 float left[480]={0},right[480];size_t pos=0;uint64_t start=os_gettime_ns();while(os_gettime_ns()-start<40000000000ULL){
 for(int i=0;i<480;i++){right[i]=pcm[(pos/3)%(size/2)]/32768.f;pos++;}
 struct obs_audio_data a={.data={(uint8_t*)left,(uint8_t*)right},.frames=480,.timestamp=os_gettime_ns()};st_filter_audio(f,&a);os_sleep_ms(10);
 }assert(f->reconnects>=2&&f->audio_chunks_sent>50);st_destroy(f);obs_set_output_source(0,NULL);obs_source_release(source);obs_data_release(settings);obs_shutdown();free(pcm);puts("PASS native speech transport recovery");return 0;}
