#include "../native-cc.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
 struct st_cc_state s={0};
 assert(!st_cc_reset(&s,""));assert(!st_cc_reset(&s,"a\"b"));
 assert(!st_cc_sequence(&s,"epoch-a",1,10001,3,10000));
 assert(st_cc_reset(&s,"epoch-a"));
 assert(st_cc_sequence(&s,"epoch-a",1,10001,3,10000));
 assert(!st_cc_sequence(&s,"epoch-a",1,10001,3,10000));
 assert(st_cc_reset(&s,"epoch-a"));assert(s.sequence==1);
 assert(!st_cc_sequence(&s,"epoch-a",0,10001,3,10000));
 assert(!st_cc_sequence(&s,"epoch-a",2,10000,3,10000));
 assert(!st_cc_sequence(&s,"epoch-a",2,40001,3,10000));
 assert(!st_cc_sequence(&s,"epoch-a",2,10001,1,10000));
 assert(!st_cc_sequence(&s,"epoch-a",2.5,10001,3,10000));
 assert(!st_cc_sequence(&s,"epoch-a",NAN,10001,3,10000));
 assert(!st_cc_sequence(&s,"epoch-b",2,10001,3,10000));
 assert(st_cc_reset(&s,"epoch-b"));assert(s.sequence==0);
 assert(!st_cc_reset(&s,"epoch-a"));
 assert(st_cc_sequence(&s,"epoch-b",3,10001,8,10000));
 assert(st_cc_supported_text("English. Numbers 123!\nNew line."));
 assert(st_cc_supported_text("Español: canción. Français: été. Português: ação. Deutsch: schön."));
 assert(!st_cc_supported_text("日本語"));assert(!st_cc_supported_text("العربية"));
 assert(!st_cc_supported_text("Hello 😀"));assert(!st_cc_supported_text("tab\ttext"));
 assert(!st_cc_supported_text("\xc0\xaf"));assert(!st_cc_supported_text("\xe0\x80\xaf"));
 assert(!st_cc_supported_text("\xed\xa0\x80"));assert(!st_cc_supported_text("\xe2\x82"));
 char big[1100];memset(big,'x',sizeof(big));big[1099]=0;assert(!st_cc_supported_text(big));
 struct st_cc_fragments f={0};
 assert(!st_cc_fragment(&f,"{\"x\":",5,true,false,false));
 /* lws can split one frame across callbacks, both carrying first-fragment. */
 const char *v=st_cc_fragment(&f,"1}",2,true,true,false);assert(v&&!strcmp(v,"{\"x\":1}"));
 assert(!st_cc_fragment(&f,"bad",3,true,false,true));
 assert(!st_cc_fragment(&f,"frame",5,false,true,true));
 assert(!st_cc_fragment(&f,"{\"x\":",5,true,false,false));
 assert(!st_cc_fragment(&f,big,sizeof(big),false,false,false));
 for(unsigned i=0;i<8;i++)st_cc_fragment(&f,big,sizeof(big),false,false,false);
 assert(!st_cc_fragment(&f,"}",1,false,true,false));
 assert(st_cc_fragment(&f,"{}",2,true,true,false));
 char nul[3]={'{',0,'}'};assert(!st_cc_fragment(&f,nul,3,true,true,false));
 puts("native CC protocol: epoch, replay, expiry, repertoire, fragmentation and recovery passed");
 return 0;
}
