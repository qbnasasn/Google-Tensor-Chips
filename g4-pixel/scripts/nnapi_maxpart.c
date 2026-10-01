// Drive the NNAPI delegate WITH options (max_number_delegated_partitions, accelerator,
// cache_dir, disallow_nnapi_cpu) via the JNI createDelegate in libtflite.so, using a
// FAKE JNIEnv (no JVM). Signature recovered by disassembly of createDelegate:
//   jlong createDelegate(env,clazz, int preference, jstring accel, jstring cache,
//        jstring token, int max_delegated_partitions, bool override_disallow_cpu,
//        bool disallow_nnapi_cpu, bool allow_fp16, long sl_handle)
// env vtable: index 169 (+0x548)=GetStringUTFChars, 170 (+0x550)=ReleaseStringUTFChars.
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>

typedef struct TfLiteModel TfLiteModel;
typedef struct Opt Opt;
typedef struct Interp Interp;
typedef struct TfLiteDelegate TfLiteDelegate;

// ---- fake JNIEnv ----
static const char* my_GetStringUTFChars(void* env, void* jstr, unsigned char* isCopy){
  if(isCopy) *isCopy = 0;
  return (const char*)jstr;            // our jstring IS a C string pointer
}
static void my_ReleaseStringUTFChars(void* env, void* jstr, const char* chars){ (void)env;(void)jstr;(void)chars; }
static void* g_table[512];

typedef long (*createDelegate_t)(void* env, void* clazz,
    int preference, void* accel, void* cache, void* token,
    int max_part, unsigned char override_disallow_cpu, unsigned char disallow_nnapi_cpu,
    unsigned char allow_fp16, long sl_handle);

int main(int argc,char**argv){
  const char* model = argc>1?argv[1]:"pd_int8.tflite";
  int max_part      = argc>2?atoi(argv[2]):1000000;      // huge = effectively unlimited
  const char* cache = argc>3?argv[3]:"/data/local/tmp/nnapi_cache_disable_cluster";
  int allow_fp16    = argc>4?atoi(argv[4]):1;            // downcast fp32->fp16 so darwinn accepts it
  int preference    = argc>5?atoi(argv[5]):2;            // NNAPI pref: 0=low_power 1=fast 2=sustained (-1=skip). 3=INVALID

  void*h=dlopen("libtflite.so",RTLD_NOW|RTLD_GLOBAL);
  if(!h){printf("dlopen fail: %s\n",dlerror());return 1;}
  TfLiteModel*(*mk)(const char*)                 = dlsym(h,"TfLiteModelCreateFromFile");
  Opt*(*optc)(void)                              = dlsym(h,"TfLiteInterpreterOptionsCreate");
  void(*setthreads)(Opt*,int)                    = dlsym(h,"TfLiteInterpreterOptionsSetNumThreads");
  void(*adddel)(Opt*,TfLiteDelegate*)            = dlsym(h,"TfLiteInterpreterOptionsAddDelegate");
  Interp*(*ic)(const TfLiteModel*,const Opt*)    = dlsym(h,"TfLiteInterpreterCreate");
  int(*alloc)(Interp*)                           = dlsym(h,"TfLiteInterpreterAllocateTensors");
  int32_t(*nin)(const Interp*)                   = dlsym(h,"TfLiteInterpreterGetInputTensorCount");
  int32_t(*nsig)(const Interp*)                  = dlsym(h,"TfLiteInterpreterGetSignatureCount");
  createDelegate_t cdel                          = dlsym(h,"Java_org_tensorflow_lite_nnapi_NnApiDelegateImpl_createDelegate");
  if(!mk||!optc||!adddel||!ic||!alloc||!cdel){printf("symbol miss mk=%p optc=%p add=%p ic=%p alloc=%p cdel=%p\n",
      (void*)mk,(void*)optc,(void*)adddel,(void*)ic,(void*)alloc,(void*)cdel);return 2;}

  // build fake env
  g_table[169]=(void*)my_GetStringUTFChars;
  g_table[170]=(void*)my_ReleaseStringUTFChars;
  void* table_ptr = g_table;
  void* env = &table_ptr;

  printf("creating NNAPI delegate: accel=google-edgetpu max_partitions=%d cache=%s disallow_nnapi_cpu=1 allow_fp16=%d preference=%d\n",max_part,cache,allow_fp16,preference);
  // override_disallow_cpu=1, disallow_nnapi_cpu=1, allow_fp16=<arg>, sl_handle=0
  long dh = cdel(env, NULL, preference,
                 (void*)"google-edgetpu", (void*)cache, (void*)"g270m",
                 max_part, 1, 1, (unsigned char)allow_fp16, 0);
  printf("createDelegate -> handle=%p\n",(void*)dh);
  if(!dh){printf("delegate create FAIL\n");return 3;}

  printf("loading %s\n",model);
  TfLiteModel*m=mk(model); if(!m){printf("model load FAIL\n");return 4;}
  Opt*o=optc(); if(setthreads)setthreads(o,1);
  adddel(o,(TfLiteDelegate*)dh);

  struct timespec a,b; clock_gettime(CLOCK_MONOTONIC,&a);
  Interp*it=ic(m,o);
  int r=it?alloc(it):-99;
  clock_gettime(CLOCK_MONOTONIC,&b);
  double s=(b.tv_sec-a.tv_sec)+(b.tv_nsec-a.tv_nsec)/1e9;
  printf("interp=%p AllocateTensors=%d COMPILE_TIME=%.2fs (60s race) sigs=%d inputs=%d\n",
         (void*)it,r,s, nsig?nsig(it):-1, nin?nin(it):-1);
  return 0;
}
