/* nn::pl shared fonts: the verified system BFTTF files laid out back to back as in the pl shared memory.
   GetSharedFontAddress points 8 bytes past each file start (after the BFTTF header), as the SDK adds the
   service-provided offset to the shared memory base; the size is the decoded font size. */
#include "runtime.h"
#include "game_data.h"
#include "host_os.h"   /* SHA-256: CommonCrypto on Darwin */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* The six files: name, size, decoded font size and SHA-256 from shared-fonts.txt in the game data folder (game_data.h;
   scripts/prepare_shared_fonts.py on a PC from the firmware's fonts, the app's setup on the device from the open font it
   bundles), read with the fonts. */
#define SHARED_FONT_COUNT 6
typedef struct{const char *path;uint64_t file_bytes,font_bytes;const char *sha256;} FontRow;
static FontRow rows[SHARED_FONT_COUNT];
static uint8_t *region;static uint64_t offsets[SHARED_FONT_COUNT];
static pthread_once_t once=PTHREAD_ONCE_INIT;
static void need(int c,const char *m){if(!c)runtime_fail(m);}
static void load(void){
 uint64_t total=0;
 for(int i=0;i<SHARED_FONT_COUNT;i++){char key[32];
  snprintf(key,sizeof key,"FONT%d_FILE",i);rows[i].path=runtime_game_text("shared-fonts.txt",key);need(!strchr(rows[i].path,'/'),"shared font file name");
  snprintf(key,sizeof key,"FONT%d_FILE_BYTES",i);rows[i].file_bytes=runtime_game_number("shared-fonts.txt",key);
  snprintf(key,sizeof key,"FONT%d_FONT_BYTES",i);rows[i].font_bytes=runtime_game_number("shared-fonts.txt",key);
  snprintf(key,sizeof key,"FONT%d_SHA256",i);rows[i].sha256=runtime_game_sha256("shared-fonts.txt",key);
  need(rows[i].file_bytes==rows[i].font_bytes+8&&rows[i].file_bytes<(64u<<20),"shared font sizes");total+=rows[i].file_bytes;}
 region=malloc(total);need(region!=NULL,"shared font memory");
 uint64_t at=0;
 for(int i=0;i<SHARED_FONT_COUNT;i++){
  /* TOTK_SHARED_FONTS=<dir>: where the files are (the app: <game>/extracted/firmware/shared-fonts). */
  char path[4096];const char *dir=getenv("TOTK_SHARED_FONTS");if(!dir||!*dir)dir="extracted/firmware/shared-fonts";
  need(snprintf(path,sizeof path,"%s/%s",dir,rows[i].path)<(int)sizeof path,"shared font path");
  FILE *f=fopen(path,"rb");need(f!=NULL,"open shared font");
  need(fread(region+at,1,rows[i].file_bytes,f)==rows[i].file_bytes&&fgetc(f)==EOF,"shared font size");fclose(f);
  uint8_t d[32];char hex[65];host_sha256(region+at,(size_t)rows[i].file_bytes,d);for(int k=0;k<32;k++)snprintf(hex+k*2,3,"%02x",d[k]);
  need(!strcmp(hex,rows[i].sha256),"shared font integrity mismatch");
  /* Z-A's main imports ScalableFontEngineHelper::Decode and hands it the address as is: it expects the BFTTF header there
    (it deobfuscates in place, or returns address + 8 when the first word is already the decoded marker), and the size with
    the header (nxnative hle_sdk16.c font_skip). The address past the header made Decode XOR the TTF as a header, no font
    face loaded, and the glyph cache's first lookup of '?' returned null (main+0x261c910, za-a8). */
#ifdef TOTK_GAME_ZA
  offsets[i]=at;
#else
  offsets[i]=at+8;
#endif
  at+=rows[i].file_bytes;
 }
 fprintf(stderr,"[totk] shared fonts loaded: %d fonts, %llu bytes\n",SHARED_FONT_COUNT,(unsigned long long)total);
}
static int valid(int type){if(type<0||type>=SHARED_FONT_COUNT)runtime_fail("invalid shared font type");pthread_once(&once,load);return type;}
static int load_state(int type){valid(type);return 1;}   /* SharedFontLoadState_Loaded */
static const void *address(int type){return region+offsets[valid(type)];}
#ifdef TOTK_GAME_ZA
static size_t size(int type){return (size_t)rows[valid(type)].file_bytes;}   /* with the 8-byte header, for Decode */
#else
static size_t size(int type){return (size_t)rows[valid(type)].font_bytes;}
#endif
#ifdef GAME_BOTW
/* SDK 7 (Breath of the Wild 1.6.0, botw.md): it asks for each font to be loaded before reading it; the fonts are loaded at
   startup. 1.6.0 also asks whether Labo VR mode is on: never on a Mac. */
static void request_load(int type){(void)type;}
static unsigned char vr_mode(void){return 0;}
static void vr_mode_change(void){fprintf(stderr,"[botw] Labo VR mode is not available\n");}
#endif
void *runtime_font_override(const char *name){
 static const struct{const char *name;void *fn;} entries[]={
  {"_ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE",load_state},
  {"_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE",address},
  {"_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE",size}
#ifdef GAME_BOTW
  ,{"_ZN2nn2pl21RequestSharedFontLoadENS0_14SharedFontTypeE",request_load},{"_ZN2nn2pl8IsVrModeEv",vr_mode},
  {"_ZN2nn2pl11BeginVrModeEv",vr_mode_change},{"_ZN2nn2pl9EndVrModeEv",vr_mode_change}
#endif
  };
 for(unsigned i=0;i<sizeof(entries)/sizeof(*entries);i++)if(!strcmp(name,entries[i].name))return entries[i].fn;return NULL;
}
