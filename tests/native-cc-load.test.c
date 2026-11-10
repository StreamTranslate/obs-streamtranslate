#include <obs.h>
#include <dlfcn.h>
#include <stdio.h>
#include <assert.h>
int main(int argc,char **argv){
 if(argc!=2)return 2;
 /* Load the host framework through an ordinary public symbol, without starting
  * OBS or touching any source/profile. Bundled deps must resolve independently. */
 printf("Host libobs version: %s\n",obs_get_version_string());
 void *module=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
 if(!module){fprintf(stderr,"%s\n",dlerror());return 1;}
 assert(dlsym(module,"obs_module_load"));assert(dlsym(module,"obs_module_ver"));
 dlclose(module);puts("Candidate plugin dlopen and required OBS symbols passed (module not activated)");return 0;
}
