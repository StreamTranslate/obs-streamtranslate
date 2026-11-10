#ifndef ST_NATIVE_CC_H
#define ST_NATIVE_CC_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#define ST_CC_MESSAGE_MAX 8192
#define ST_CC_TEXT_MAX 1024
#define ST_CC_EPOCH_MAX 64
/* Character repertoire follows OBS libcaption's EIA608 tables. No transliteration. */
static const uint32_t st_cc_glyphs[] = {0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27,0x28,0x29,0x2a,0x2b,0x2c,0x2d,0x2e,0x2f,0x30,0x31,0x32,0x33,0x34,0x35,0x36,0x37,0x38,0x39,0x3a,0x3b,0x3c,0x3d,0x3e,0x3f,0x40,0x41,0x42,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4a,0x4b,0x4c,0x4d,0x4e,0x4f,0x50,0x51,0x52,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x5b,0x5c,0x5d,0x5e,0x5f,0x61,0x62,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6a,0x6b,0x6c,0x6d,0x6e,0x6f,0x70,0x71,0x72,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x7b,0x7c,0x7d,0x7e,0xa0,0xa1,0xa2,0xa3,0xa4,0xa5,0xa6,0xa9,0xab,0xae,0xb0,0xbb,0xbd,0xbf,0xc0,0xc1,0xc2,0xc3,0xc4,0xc5,0xc7,0xc8,0xc9,0xca,0xcb,0xcc,0xcd,0xce,0xcf,0xd1,0xd2,0xd3,0xd4,0xd5,0xd6,0xd8,0xd9,0xda,0xdb,0xdc,0xdf,0xe0,0xe1,0xe2,0xe3,0xe4,0xe5,0xe7,0xe8,0xe9,0xea,0xeb,0xec,0xed,0xee,0xef,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,0xfa,0xfb,0xfc,0x2014,0x2018,0x2019,0x201c,0x201d,0x2022,0x2120,0x2122,0x250c,0x2510,0x2514,0x2518,0x2588,0x266a};
struct st_cc_state {char epoch[65];uint64_t sequence;char previous[8][65];unsigned previous_count;};
struct st_cc_fragments {char bytes[ST_CC_MESSAGE_MAX+1];size_t used;bool dropping,active;};
static bool st_cc_epoch_valid(const char *epoch) {
 if(!epoch||!*epoch||strlen(epoch)>ST_CC_EPOCH_MAX)return false;
 for(const unsigned char *p=(const unsigned char*)epoch;*p;p++)if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='_'||*p=='-'))return false;
 return true;
}
static bool st_cc_reset(struct st_cc_state *state,const char *epoch) {
 if(!st_cc_epoch_valid(epoch))return false;
 if(!strcmp(state->epoch,epoch))return true;
 for(unsigned i=0;i<state->previous_count;i++)if(!strcmp(state->previous[i],epoch))return false;
 if(*state->epoch){if(state->previous_count==8){memmove(state->previous,state->previous+1,7*65);state->previous_count--;}strcpy(state->previous[state->previous_count++],state->epoch);}
 strcpy(state->epoch,epoch);state->sequence=0;return true;
}
static bool st_cc_sequence(struct st_cc_state *state,const char *epoch,double seq,double expires,double duration,uint64_t now) {
 if(!st_cc_epoch_valid(epoch)||strcmp(state->epoch,epoch)||!isfinite(seq)||seq<1||seq>9007199254740991.0||floor(seq)!=seq||seq<=(double)state->sequence||!isfinite(expires)||floor(expires)!=expires||expires<=(double)now||expires>(double)now+30000||!isfinite(duration)||duration<2||duration>8)return false;
 state->sequence=(uint64_t)seq;return true;
}
static bool st_cc_supported_text(const char *text) {
 if(!text||!*text||strlen(text)>ST_CC_TEXT_MAX)return false;
 const unsigned char *p=(const unsigned char *)text;
 while(*p){uint32_t cp;unsigned n;
  if(*p<0x80){cp=*p;n=1;}
  else if(*p>=0xc2&&*p<=0xdf){cp=*p&31;n=2;}
  else if(*p>=0xe0&&*p<=0xef){cp=*p&15;n=3;}
  else return false;
  for(unsigned i=1;i<n;i++){if(!p[i]||(p[i]&0xc0)!=0x80)return false;cp=(cp<<6)|(p[i]&63);}
  if((n==2&&cp<0x80)||(n==3&&cp<0x800)||(cp>=0xd800&&cp<=0xdfff))return false;
  bool ok=cp=='\n';for(size_t i=0;i<sizeof(st_cc_glyphs)/sizeof(st_cc_glyphs[0]);i++)if(cp==st_cc_glyphs[i]){ok=true;break;}if(!ok)return false;p+=n;
 }
 return true;
}
/* Returns complete message only at the end; oversize/binary frames are discarded. */
static const char *st_cc_fragment(struct st_cc_fragments *f,const void *data,size_t size,bool first,bool final,bool binary) {
 if(!f->active){f->used=0;f->dropping=binary||!first;f->active=true;}
 if(!data||binary||size>ST_CC_MESSAGE_MAX-f->used)f->dropping=true;
 if(!f->dropping){memcpy(f->bytes+f->used,data,size);f->used+=size;}
 if(!final)return NULL;
 f->active=false;
 if(f->dropping){f->used=0;return NULL;}
 f->bytes[f->used]=0;
 /* Embedded NUL cannot be safely handed to a C-string JSON parser. */
 if(memchr(f->bytes,0,f->used)){f->used=0;return NULL;}
 return f->bytes;
}
#endif
