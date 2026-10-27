#include "../plugin.c"
#include <assert.h>
int main(void) {
 assert(obs_startup("en-US",NULL,NULL));
 struct obs_audio_info ai={.samples_per_sec=48000,.speakers=SPEAKERS_STEREO};
 assert(obs_reset_audio(&ai));
 obs_data_t *settings=obs_data_create();
 st_get_defaults(settings);
 obs_data_set_bool(settings,"stereo_split",true); /* old saved scene */
 struct st_filter f={0};
 pthread_mutex_init(&f.qlock,NULL);
 st_update(&f,settings);
 assert(!f.stereo_split);
 obs_properties_t *props=st_get_properties(NULL);
 assert(!obs_properties_get(props,"stereo_split"));
 assert(obs_properties_get(props,"plugin_key"));
 assert(obs_properties_get(props,"stop_stream"));
 assert(obs_properties_get(props,"test_connection"));
 assert(!obs_properties_get(props,"reconnect"));
 st_build_resampler(&f);
 assert(f.resampler && !f.sending_stereo);
 float left[480]={0},right[480];
 for(int i=0;i<480;i++)right[i]=0.5f;
 const uint8_t *in[MAX_AV_PLANES]={(const uint8_t*)left,(const uint8_t*)right};
 uint8_t *out[MAX_AV_PLANES]={0};uint32_t frames=0;uint64_t offset=0;
 assert(audio_resampler_resample(f.resampler,out,&frames,&offset,in,480));
 assert(frames>0 && frames<=480 && out[0]);
 int peak=0;for(uint32_t i=0;i<frames;i++){int x=((int16_t*)out[0])[i];if(x>peak)peak=x;}
 assert(peak>1000); /* right-only input survives the mono downmix */
 audio_resampler_destroy(f.resampler);obs_properties_destroy(props);
 obs_data_release(settings);pthread_mutex_destroy(&f.qlock);obs_shutdown();
 puts("PASS public UI, legacy stereo setting ignored, mono output, right-channel capture");
}
