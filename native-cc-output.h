#ifndef ST_NATIVE_CC_OUTPUT_H
#define ST_NATIVE_CC_OUTPUT_H
#include "native-cc.h"
#include <time.h>

/* Optional frontend entry points are resolved at runtime: an unsupported OBS host
 * continues capturing audio normally. No startup/loading dependency is added. */
static void *st_cc_symbol(const char *name)
{
#ifdef _WIN32
 HMODULE frontend=GetModuleHandleA("obs-frontend-api.dll");
 void *symbol=frontend?(void*)GetProcAddress(frontend,name):NULL;
 if(!symbol){HMODULE obs=GetModuleHandleA("obs.dll");symbol=obs?(void*)GetProcAddress(obs,name):NULL;}
 return symbol;
#else
 return dlsym(RTLD_DEFAULT,name);
#endif
}
static uint64_t st_cc_wall_ms(void){struct timespec ts;timespec_get(&ts,TIME_UTC);return (uint64_t)ts.tv_sec*1000+(uint64_t)ts.tv_nsec/1000000;}
static bool st_cc_json_type(obs_data_t *json,const char *name,enum obs_data_type type)
{
 obs_data_item_t *item=obs_data_item_byname(json,name);if(!item)return false;
 bool valid=obs_data_item_gettype(item)==type;obs_data_item_release(&item);return valid;
}
static const char *st_cc_submit(const char *text,double duration)
{
 typedef obs_output_t *(*get_output_fn)(void);
 typedef void (*caption_fn)(obs_output_t *,const char *,double);
 get_output_fn get_output=(get_output_fn)st_cc_symbol("obs_frontend_get_streaming_output");
 caption_fn caption=(caption_fn)st_cc_symbol("obs_output_output_caption_text2");
 if(!get_output||!caption)return "unsupported_output";
 obs_output_t *output=get_output();
 if(!output)return "not_streaming";
 const char *status="unsupported_output";
 if(!obs_output_active(output)){status="not_streaming";goto done;}
 obs_service_t *service=obs_output_get_service(output);
 if(!service)goto done;
 obs_data_t *settings=obs_service_get_settings(service);
 bool twitch=settings&&!strcmp(obs_data_get_string(settings,"service"),"Twitch");
 if(settings)obs_data_release(settings);
 /* Restrict the first implementation to Twitch's built-in service and H.264.
  * Custom RTMP and other service outputs must be explicitly tested later. */
 obs_encoder_t *encoder=obs_output_get_video_encoder(output);
 const char *codec=encoder?obs_encoder_get_codec(encoder):NULL;
 if(!twitch||!codec||strcmp(codec,"h264"))goto done;
 /* OBS interprets duration as a minimum hold before dequeuing the NEXT caption,
  * not as an expiry. Long server hints accumulate seconds of stale captions.
  * Zero lets the next current caption replace this one on the next video frame;
  * the displayed caption still remains until a replacement arrives. */
 (void)duration;
 caption(output,text,0.0);status="submitted";
done:
 obs_output_release(output);return status;
}
static void st_cc_ack(struct st_filter *f,const char *epoch,uint64_t seq,const char *status)
{
 char *ack=f->cc_acks[(f->cc_ack_read+f->cc_ack_count)%8];
 if(f->cc_ack_count==8){f->cc_ack_read=(f->cc_ack_read+1)%8;f->cc_ack_count--;ack=f->cc_acks[(f->cc_ack_read+f->cc_ack_count)%8];}
 snprintf(ack,256,"{\"type\":\"native_cc_ack\",\"v\":1,\"epoch\":\"%s\",\"seq\":%" PRIu64 ",\"status\":\"%s\"}",epoch,seq,status);f->cc_ack_count++;
 if(f->wsi)lws_callback_on_writable(f->wsi);
}
static void st_cc_receive(struct st_filter *f,const char *message)
{
 if(!f->native_twitch_cc||f->speaker!=0||f->user_paused||!obs_source_enabled(f->context))return;
 /* Embedded JSON NUL is not representable by the downstream C-string API. */
 if(strstr(message,"\\u0000"))return;
 obs_data_t *json=obs_data_create_from_json(message);if(!json)return;
 if(!st_cc_json_type(json,"type",OBS_DATA_STRING)||!st_cc_json_type(json,"v",OBS_DATA_NUMBER)||obs_data_get_double(json,"v")!=1||!st_cc_json_type(json,"epoch",OBS_DATA_STRING))goto done;
 const char *type=obs_data_get_string(json,"type"),*epoch=obs_data_get_string(json,"epoch");
 if(!strcmp(type,"native_cc_reset")){st_cc_reset(&f->cc_state,epoch);goto done;}
 if(strcmp(type,"native_cc_caption")||!st_cc_json_type(json,"seq",OBS_DATA_NUMBER)||!st_cc_json_type(json,"expiresAt",OBS_DATA_NUMBER)||!st_cc_json_type(json,"duration",OBS_DATA_NUMBER)||!st_cc_json_type(json,"text",OBS_DATA_STRING))goto done;
 if(!st_cc_sequence(&f->cc_state,epoch,obs_data_get_double(json,"seq"),obs_data_get_double(json,"expiresAt"),obs_data_get_double(json,"duration"),st_cc_wall_ms()))goto done;
 const char *text=obs_data_get_string(json,"text");
 const char *status=st_cc_supported_text(text)?st_cc_submit(text,obs_data_get_double(json,"duration")):"unsupported_text";
 st_cc_ack(f,epoch,f->cc_state.sequence,status);
done:
 obs_data_release(json);
}
#endif
