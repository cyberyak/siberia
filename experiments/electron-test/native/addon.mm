// Node-API addon: moves IOSurfaces between Electron and other processes.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <mach/mach_time.h>
#include <node_api.h>
#include <pthread.h>
#include <map>
#include "proto.h"
static id<MTLDevice> dev; static id<MTLCommandQueue> q; static mach_port_t outPort;
struct Ring { IOSurfaceRef s[6]; id<MTLTexture> t[6]; int n=0; int w=0,h=0; uint32_t next=0; uint32_t seq=0; bool announced=false; uint32_t base=1; };
static std::map<int, Ring> rings;
static double blitTotal=0; static long blitCount=0; static double blitMax=0;
#define ARG(n) size_t argc=n; napi_value argv[n]; napi_get_cb_info(env,info,&argc,argv,NULL,NULL)
static IOSurfaceRef surfaceArg(napi_env env, napi_value v){ void*d; size_t l; napi_get_buffer_info(env,v,&d,&l); return l>=8? *(IOSurfaceRef*)d : NULL; }
static napi_value num(napi_env env,double d){ napi_value r; napi_create_double(env,d,&r); return r; }
static napi_value str(napi_env env,const char*s){ napi_value r; napi_create_string_utf8(env,s,NAPI_AUTO_LENGTH,&r); return r; }
static void setp(napi_env env,napi_value o,const char*k,napi_value v){ napi_set_named_property(env,o,k,v); }
static napi_value inspect(napi_env env,napi_callback_info info){ ARG(1); IOSurfaceRef s=surfaceArg(env,argv[0]); napi_value o; napi_create_object(env,&o); if(!s) return o;
  OSType f=IOSurfaceGetPixelFormat(s); char cc[5]={(char)(f>>24),(char)(f>>16),(char)(f>>8),(char)f,0};
  setp(env,o,"id",num(env,IOSurfaceGetID(s))); setp(env,o,"width",num(env,IOSurfaceGetWidth(s))); setp(env,o,"height",num(env,IOSurfaceGetHeight(s)));
  setp(env,o,"bytesPerRow",num(env,IOSurfaceGetBytesPerRow(s))); setp(env,o,"format",str(env,cc)); setp(env,o,"planes",num(env,IOSurfaceGetPlaneCount(s))); setp(env,o,"allocSize",num(env,IOSurfaceGetAllocSize(s))); return o; }
static MTLPixelFormat fmt(OSType f){ return f=='RGhA'?MTLPixelFormatRGBA16Float: f=='RGBA'?MTLPixelFormatRGBA8Unorm:MTLPixelFormatBGRA8Unorm; }
static id<MTLTexture> wrap(IOSurfaceRef s){ MTLTextureDescriptor*d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt(IOSurfaceGetPixelFormat(s)) width:IOSurfaceGetWidth(s) height:IOSurfaceGetHeight(s) mipmapped:NO]; d.usage=MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget; d.storageMode=MTLStorageModeShared; return [dev newTextureWithDescriptor:d iosurface:s plane:0]; }
static napi_value connectOut(napi_env env,napi_callback_info info){ if(!dev){dev=MTLCreateSystemDefaultDevice(); q=[dev newCommandQueue];} outPort=lookup_service(SVC_OUT); napi_value r; napi_get_boolean(env,outPort!=MACH_PORT_NULL,&r); return r; }
// forward(stream, chromiumSurface): GPU-copies into our ring, so Chromium's surface can go back at once.
static napi_value forward(napi_env env,napi_callback_info info){ ARG(2); int32_t stream; napi_get_value_int32(env,argv[0],&stream); IOSurfaceRef src=surfaceArg(env,argv[1]); if(!src||!outPort) return num(env,-1);
  Ring&r=rings[stream]; int w=(int)IOSurfaceGetWidth(src),h=(int)IOSurfaceGetHeight(src);
  if(r.w!=w||r.h!=h){ r.w=w;r.h=h;r.n=4;r.base=(uint32_t)(stream*10+1);
    for(int i=0;i<r.n;i++){ r.s[i]=IOSurfaceCreate((__bridge CFDictionaryRef)@{(id)kIOSurfaceWidth:@(w),(id)kIOSurfaceHeight:@(h),(id)kIOSurfaceBytesPerElement:@(IOSurfaceGetBytesPerElement(src)),(id)kIOSurfacePixelFormat:@(IOSurfaceGetPixelFormat(src))}); r.t[i]=wrap(r.s[i]); send_surface(outPort,r.s[i],r.base+i);} }
  @autoreleasepool { uint64_t t0=mach_absolute_time(); id<MTLTexture> st=wrap(src); int i=r.next++%r.n; id<MTLCommandBuffer> c=[q commandBuffer]; id<MTLBlitCommandEncoder> e=[c blitCommandEncoder]; [e copyFromTexture:st toTexture:r.t[i]]; [e endEncoding]; [c commit]; [c waitUntilCompleted];
    mach_timebase_info_data_t tb; mach_timebase_info(&tb); double ms=(mach_absolute_time()-t0)*(double)tb.numer/tb.denom/1e6; blitTotal+=ms; blitCount++; if(ms>blitMax)blitMax=ms;
    send_frame(outPort,r.base+i,r.seq++); return num(env,ms);} }
// forwardTiles(surface, cols, rows): splits one offscreen frame into separate streams with a single GPU pass.
static napi_value forwardTiles(napi_env env,napi_callback_info info){ ARG(3); IOSurfaceRef src=surfaceArg(env,argv[0]); int32_t cols,rows; napi_get_value_int32(env,argv[1],&cols); napi_get_value_int32(env,argv[2],&rows); if(!src||!outPort) return num(env,-1);
  int cw=(int)IOSurfaceGetWidth(src)/cols, ch=(int)IOSurfaceGetHeight(src)/rows; @autoreleasepool { uint64_t t0=mach_absolute_time(); id<MTLTexture> st=wrap(src); id<MTLCommandBuffer> c=[q commandBuffer]; id<MTLBlitCommandEncoder> e=[c blitCommandEncoder]; uint32_t sids[32], seqs[32]; int n=0;
  for(int y=0;y<rows;y++) for(int x=0;x<cols;x++){ int stream=y*cols+x+1; Ring&r=rings[stream]; if(r.w!=cw||r.h!=ch){ r.w=cw;r.h=ch;r.n=4;r.base=(uint32_t)(stream*10+1); for(int i=0;i<r.n;i++){ r.s[i]=IOSurfaceCreate((__bridge CFDictionaryRef)@{(__bridge id)kIOSurfaceWidth:@(cw),(__bridge id)kIOSurfaceHeight:@(ch),(__bridge id)kIOSurfaceBytesPerElement:@(IOSurfaceGetBytesPerElement(src)),(__bridge id)kIOSurfacePixelFormat:@(IOSurfaceGetPixelFormat(src))}); r.t[i]=wrap(r.s[i]); send_surface(outPort,r.s[i],r.base+i);} }
    int i=r.next++%r.n; [e copyFromTexture:st sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(x*cw,y*ch,0) sourceSize:MTLSizeMake(cw,ch,1) toTexture:r.t[i] destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)]; sids[n]=r.base+i; seqs[n++]=r.seq++; }
  [e endEncoding]; [c commit]; [c waitUntilCompleted]; mach_timebase_info_data_t tb; mach_timebase_info(&tb); double ms=(mach_absolute_time()-t0)*(double)tb.numer/tb.denom/1e6; blitTotal+=ms; blitCount++; if(ms>blitMax)blitMax=ms; for(int i=0;i<n;i++) send_frame(outPort,sids[i],seqs[i]); return num(env,ms);} }
static napi_value stats(napi_env env,napi_callback_info info){ napi_value o; napi_create_object(env,&o); setp(env,o,"blitAvgMs",num(env,blitCount?blitTotal/blitCount:0)); setp(env,o,"blitMaxMs",num(env,blitMax)); setp(env,o,"blits",num(env,blitCount)); return o; }
// listen(cb): delivers (surfaceBuffer, sid, w, h, seq) for every incoming frame on the JS thread.
struct Ev{ IOSurfaceRef s; uint32_t sid,w,h,seq; };
static napi_threadsafe_function tsfn; static std::map<uint32_t,IOSurfaceRef> inS; static mach_port_t inPort;
static void callJs(napi_env env,napi_value cb,void*,void*data){ Ev*e=(Ev*)data; if(env&&cb){ napi_value args[5]; void*p; napi_create_buffer_copy(env,8,&e->s,&p,&args[0]); args[1]=num(env,e->sid); args[2]=num(env,e->w); args[3]=num(env,e->h); args[4]=num(env,e->seq); napi_value g; napi_get_undefined(env,&g); napi_call_function(env,g,cb,5,args,NULL);} delete e; }
static void* inLoop(void*){ rx_buf m; for(;;){ kern_return_t k=mach_msg(&m.s.h,MACH_RCV_MSG,0,sizeof m,inPort,0,0); if(k) continue;
    if(m.s.h.msgh_id==MSG_SURFACE){ inS[m.s.sid]=IOSurfaceLookupFromMachPort(m.s.port.name); mach_port_deallocate(mach_task_self(),m.s.port.name); continue; }
    IOSurfaceRef s=inS[m.f.sid]; if(!s) continue; Ev*e=new Ev{s,m.f.sid,(uint32_t)IOSurfaceGetWidth(s),(uint32_t)IOSurfaceGetHeight(s),m.f.seq}; napi_call_threadsafe_function(tsfn,e,napi_tsfn_nonblocking); } }
static napi_value listen_(napi_env env,napi_callback_info info){ ARG(1); napi_value name=str(env,"siberia"); napi_create_threadsafe_function(env,argv[0],NULL,name,0,1,NULL,NULL,NULL,callJs,&tsfn); inPort=register_service(SVC_IN); if(!inPort){ napi_throw_error(env,NULL,"register failed"); return NULL;} pthread_t t; pthread_create(&t,NULL,inLoop,NULL); return num(env,1); }
NAPI_MODULE_INIT(){ napi_value f; 
#define X(n,fn) napi_create_function(env,n,NAPI_AUTO_LENGTH,fn,NULL,&f); napi_set_named_property(env,exports,n,f);
  X("inspect",inspect) X("connectOut",connectOut) X("forward",forward) X("forwardTiles",forwardTiles) X("stats",stats) X("listen",listen_) return exports; }
