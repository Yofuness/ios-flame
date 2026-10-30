// SPDX-License-Identifier: MIT
// Native Metal API frontend over the bridge (bridge/NativeMetalObjects.h): the metal2vulkan runtime on Turnip, on the Adreno 740 through
// NativeAdreno. Same shape as the offline frontend: the device is a dynamically allocated subclass of the target's own _MTLDevice that
// only adds this file's implementations; every other object is an own class answering the Metal protocol methods it implements.
// Every result comes from the runtime and the GPU: a refused call returns nil/NO with the runtime's reason, an encoder call the runtime
// refuses fails its command buffer (status Error, handlers run) instead of being dropped. A selector this frontend does not implement is
// reported through the missing-selector hook before the usual exception, so a hardware run names the gap.
// Shared events work within this process (no cross-process handle yet). Not yet implemented here: heaps, argument buffers, indirect commands, drawables/presentation.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurfaceRef.h>
#import <objc/runtime.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <pthread.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <sys/mman.h>
#include <CommonCrypto/CommonDigest.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <mach/mach_time.h>
#include <execinfo.h>
#include <mach/mach.h>
#include "bridge/NativeMetalObjects.h"
#include "NativeBridgeMetalFrontend.h"

extern "C" void sys_dcache_flush(void *start, size_t length);

static NmbDevice *gDevice;
static id gDeviceObject;
static uint64_t gRegistryID;
static NSString *gName;
static void (*gMissing)(const char *, const char *);

// What this device has been asked and refused, for the plugin's status line (native_bridge_metal_status): commits, render/compute pipelines made,
// refusals, and the last two refusals' texts. Counters only; nothing here changes a result.
static std::atomic<uint64_t> gCommits,gCommitFailures,gPipelines,gProblems,gCompleted;
// What the render server actually asks of the GPU (int-141: 59 commits in 200 s, and a desktop that behaves like CPU compositing): render passes,
// draws, the pixels of their first colour attachment, compute dispatches, blits and the largest pass (pixels).
static std::atomic<uint64_t> gPasses,gDraws,gPassPixels,gDispatches,gBlits,gBiggestPass,gFullScreenPasses;
// Where the time goes on the CPU side of the bridge: pipeline, library and function creation (count, total and slowest, in microseconds), and the
// last commits with what each held and how long the GPU took (commit to completion).
static uint64_t nowUs() {return clock_gettime_nsec_np(CLOCK_UPTIME_RAW)/1000;}
struct Timed {std::atomic<uint64_t> count{0},total{0},slowest{0};
 void add(uint64_t began){const uint64_t took=nowUs()-began;count++;total+=took;uint64_t seen=slowest.load();while(took>seen && !slowest.compare_exchange_weak(seen,took)){}}};
static Timed gTimePipeline,gTimeLibrary,gTimeFunction,gTimeEncodeToCommit;
static Timed gTimeWaitScheduled,gTimeWaitCompleted;
static Timed gTimeResourceWait;
static std::atomic<uint64_t> gResourceChecks,gResourceSkipped;
static Timed gTimeSurfaceFlush;
static std::atomic<uint64_t> gSurfaceFlushBytes,gSurfaceFlushDuplicates;
struct EncodedWork {uint32_t passes,draws,dispatches,blits;uint64_t pixels;};
struct CommitRecord {uint64_t committedUs=0,callbackUs=0,ageUs=0;int64_t scheduledUs=-1;uint32_t index=0;EncodedWork work={};};
static CommitRecord gCommitRing[32];
static std::mutex gCommitLock;
static std::mutex gProblemLock;
static char gLastProblems[2][200];
static void noteProblem(NSString *what) {
 gProblems++;
 std::lock_guard<std::mutex> lock(gProblemLock);
 memcpy(gLastProblems[1],gLastProblems[0],sizeof gLastProblems[0]);
 strlcpy(gLastProblems[0],what.UTF8String?:"?",sizeof gLastProblems[0]);
}
extern "C" void sm2_logs(const char *);
#define NBLOG(...) do{NSString *nbText=[NSString stringWithFormat:__VA_ARGS__];sm2_logs(nbText.UTF8String);noteProblem(nbText);}while(0)
// 0106 (B29, diagnostic): 1-byte texel textures' row pitch, logged without counting as a problem. Off: M2V_B29_LOG=0.
#define B29LOG(...) do{const char *nbOff=getenv("M2V_B29_LOG");if(!nbOff||strcmp(nbOff,"0"))sm2_logs([NSString stringWithFormat:__VA_ARGS__].UTF8String);}while(0)
// The pixel formats textures were asked for (first eight distinct), with how many were refused: native_bridge_metal_formats.
static uint32_t gFormats[8];static unsigned gFormatCount;static std::atomic<uint64_t> gTextureRefusals;
static void noteFormat(uint32_t format,bool made) {
 std::lock_guard<std::mutex> lock(gProblemLock);
 if(!made)gTextureRefusals++;
 for(unsigned i=0;i<gFormatCount;i++)if(gFormats[i]==format)return;
 if(gFormatCount<8)gFormats[gFormatCount++]=format;
}
static NSError *nbError(NSString *why) {
 return [NSError errorWithDomain:@"org.blu-sharky.native-metal" code:-1 userInfo:@{NSLocalizedDescriptionKey:why?:@"refused"}];
}
static NSString *lastError() {return [NSString stringWithUTF8String:nmb_last_error()];}
// The render pipeline journal: one line per render pipeline creation (when, on which thread, how long, the outcome, both functions with
// every constant value, the attachments, blending and vertex descriptor) appended to pipelines.log in the runtime's cache directory, which
// the board log exports with the caches (CacheExport.h): every pipeline a boot made, or was refused, can be made again on the host.
static std::mutex gJournalLock;
static std::string gJournalPath;
static uint64_t gJournalBegan;
static void journal(const std::string &line) {
 if(gJournalPath.empty())return;
 std::lock_guard<std::mutex> lock(gJournalLock);
 if(FILE *f=fopen(gJournalPath.c_str(),"a")){fwrite(line.data(),1,line.size(),f);fputc('\n',f);fclose(f);}
}

@interface NBObject : NSObject
@property(atomic,copy) NSString *label;
@end
@implementation NBObject
- (id<MTLDevice>)device {return gDeviceObject;}
// Metal SPI (Metal Performance Shaders asks every encoder and command buffer): an identity for its traces.
- (uint64_t)globalTraceObjectID {return (uint64_t)(uintptr_t)self;}
- (void)doesNotRecognizeSelector:(SEL)selector {
 if(gMissing)gMissing(class_getName(object_getClass(self)),sel_getName(selector));
 noteProblem([NSString stringWithFormat:@"missing %s %s",class_getName(object_getClass(self)),sel_getName(selector)]);
 [super doesNotRecognizeSelector:selector];
}
@end

// ---- resources ----
// The wait takes a strong snapshot of already-committed buffers; new commits require the
// caller's usual resource synchronization. Storage dependency checks are in the runtime.
static NSMutableSet *gInFlight;
static void waitForResource(id resource);
// Resource SPI the runtime has no state for: no protected content (Metal's default options), and no owner transfer
// (memory attribution is the kernel driver's, by the importing task), refused as not supported.
#define NB_RESOURCE_SPI \
- (uint64_t)protectionOptions {return 0;} \
- (id<MTLHeap>)heap {return _parentHeap;} \
- (BOOL)isAliasable {return NO;} \
- (int)setOwnerWithIdentity:(unsigned)identity {(void)identity;return 46; /* KERN_NOT_SUPPORTED */} \
- (void)waitUntilComplete {waitForResource(self);}
@interface NBBuffer : NBObject
@property(nonatomic) NmbBuffer *handle;
@property(nonatomic) MTLResourceOptions options;
@property(nonatomic,copy) void (^deallocator)(void *,NSUInteger);
@property(nonatomic) void *wrapped;
@property(nonatomic) IOSurfaceRef surfaceOwner; // keeps a shared import's CPU mapping alive
@property(nonatomic,strong) id<MTLHeap> parentHeap;   // the heap that made the buffer, nil otherwise
@property(nonatomic) NSUInteger heapOffset;
@end
@interface NBTexture : NBObject
@property(nonatomic) NmbTexture *handle;
@property(nonatomic) NmbTextureDescriptor descriptor;
@property(nonatomic,strong) NBBuffer *buffer;
@property(nonatomic) IOSurfaceRef surface;
@property(nonatomic) NSUInteger offset,rowBytes;
@property(nonatomic,strong) id<MTLTexture> parent;
@property(nonatomic,strong) id<MTLHeap> parentHeap;
@property(nonatomic) NSUInteger heapOffset;
@property(nonatomic) IOSurfaceRef planarSurface;
@property(nonatomic) uint32_t planarSeed;
@end
static void planarFill(NBTexture *t);
@implementation NBBuffer
- (void)dealloc {
 const NSUInteger length=nmb_buffer_length(_handle);nmb_buffer_release(_handle);
 if(_deallocator)_deallocator(_wrapped,length);
 if(_surfaceOwner)CFRelease(_surfaceOwner);
}
- (NSUInteger)length {return nmb_buffer_length(_handle);}
- (void *)contents {return nmb_buffer_contents(_handle);}
- (uint64_t)gpuAddress {return nmb_buffer_gpu_address(_handle);}
- (MTLResourceOptions)resourceOptions {return _options;}
- (MTLStorageMode)storageMode {return (MTLStorageMode)((_options&MTLResourceStorageModeMask)>>MTLResourceStorageModeShift);}
- (MTLCPUCacheMode)cpuCacheMode {return (MTLCPUCacheMode)((_options&MTLResourceCPUCacheModeMask)>>MTLResourceCPUCacheModeShift);}
- (MTLHazardTrackingMode)hazardTrackingMode {return (MTLHazardTrackingMode)((_options&MTLResourceHazardTrackingModeMask)>>MTLResourceHazardTrackingModeShift);}
- (void)didModifyRange:(NSRange)range {(void)range;}
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state {
 const uint64_t answer=nmb_buffer_set_purgeable_state(_handle,state);
 if(!answer)NBLOG(@"native-metal: buffer setPurgeableState refused: %@",lastError());
 return (MTLPurgeableState)answer;
}
- (NSUInteger)allocatedSize {return nmb_buffer_length(_handle);}
- (IOSurfaceRef)iosurface {return nullptr;}   // a buffer is never the backing of a surface here (surfaces back textures)
NB_RESOURCE_SPI
@end
// Reuse only an import still held by a texture/view or pending command. Weak
// values never extend a GPU allocation's lifetime. The strong buffer snapshot
// and its retained surface prevent object/address reuse during a hit. A new
// mapping, allocation identity or size needs a separate import; ordinary
// caller-owned no-copy buffers do not enter this cache.
static NSMapTable<NSValue *,NBBuffer *> *gSurfaceImports;
static std::mutex gSurfaceImportLock;
static NBBuffer *surfaceBuffer(IOSurfaceRef surface,void *base,size_t bytes) {
 struct Key {uint64_t identity,address,bytes;};
 const Key value={IOSurfaceGetID(surface),(uintptr_t)base,bytes};
 NSValue *key=value.identity?[NSValue valueWithBytes:&value objCType:@encode(Key)]:nil;
 std::lock_guard<std::mutex> lock(gSurfaceImportLock);
 if(!gSurfaceImports)gSurfaceImports=[NSMapTable strongToWeakObjectsMapTable];
 NBBuffer *buffer=key?[gSurfaceImports objectForKey:key]:nil;
 if(buffer)return buffer;
 NmbBuffer *handle=nmb_buffer_new_no_copy(gDevice,base,bytes,MTLResourceStorageModeShared);
 if(!handle){NBLOG(@"native-metal: surface import refused: %@",lastError());return nil;}
 buffer=[NBBuffer new];buffer.handle=handle;buffer.options=MTLResourceStorageModeShared;
 buffer.surfaceOwner=(IOSurfaceRef)CFRetain(surface);
 if(key) {
  // Bound metadata without retaining old frames. Evicting an association is
  // harmless: its owners still retain the actual import until their own end.
  if(gSurfaceImports.count>=256) {
   for(NSValue *old in gSurfaceImports.keyEnumerator.allObjects)
    if(![gSurfaceImports objectForKey:old])[gSurfaceImports removeObjectForKey:old];
   if(gSurfaceImports.count>=256)[gSurfaceImports removeObjectForKey:gSurfaceImports.keyEnumerator.nextObject];
  }
  [gSurfaceImports setObject:buffer forKey:key];
 }
 return buffer;
}
static bool textureDescriptor(MTLTextureDescriptor *d,NmbTextureDescriptor *out) {
 if(!d)return false;
 *out={(uint64_t)d.textureType,(uint64_t)d.pixelFormat,d.width,d.height,d.depth,d.mipmapLevelCount,d.sampleCount,d.arrayLength,(uint64_t)d.usage,(uint64_t)d.resourceOptions};
 return true;
}
@implementation NBTexture
- (void)dealloc {nmb_texture_release(_handle);if(_surface)CFRelease(_surface);if(_planarSurface)CFRelease(_planarSurface);}
- (MTLTextureType)textureType {return (MTLTextureType)_descriptor.type;}
- (MTLPixelFormat)pixelFormat {return (MTLPixelFormat)_descriptor.format;}
- (NSUInteger)width {return _descriptor.width;}
- (NSUInteger)height {return _descriptor.height;}
- (NSUInteger)depth {return _descriptor.depth;}
- (NSUInteger)mipmapLevelCount {return _descriptor.levels;}
- (NSUInteger)sampleCount {return _descriptor.samples;}
- (NSUInteger)arrayLength {return _descriptor.array_length;}
- (MTLTextureUsage)usage {return (MTLTextureUsage)_descriptor.usage;}
- (MTLResourceOptions)resourceOptions {return (MTLResourceOptions)_descriptor.options;}
- (MTLStorageMode)storageMode {return (MTLStorageMode)((_descriptor.options&MTLResourceStorageModeMask)>>MTLResourceStorageModeShift);}
- (MTLCPUCacheMode)cpuCacheMode {return (MTLCPUCacheMode)((_descriptor.options&MTLResourceCPUCacheModeMask)>>MTLResourceCPUCacheModeShift);}
- (BOOL)isFramebufferOnly {return NO;}
- (IOSurfaceRef)iosurface {return _surface;}
- (NSUInteger)iosurfacePlane {return 0;}
- (id<MTLBuffer>)buffer {return _surface?nil:(id<MTLBuffer>)_buffer;}
- (NSUInteger)bufferOffset {return _offset;}
- (NSUInteger)bufferBytesPerRow {return _rowBytes;}
- (id<MTLTexture>)parentTexture {return _parent;}
- (NSUInteger)rotation {return 0;}
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state {
 const uint64_t answer=nmb_texture_set_purgeable_state(_handle,state);
 if(!answer)NBLOG(@"native-metal: texture setPurgeableState refused: %@",lastError());
 return (MTLPurgeableState)answer;
}
NB_RESOURCE_SPI
- (id<MTLTexture>)view:(NmbTexture *)handle {
 if(!handle){NBLOG(@"native-metal: texture view refused: %@",lastError());return nil;}
 NBTexture *view=[NBTexture new];view.handle=handle;NmbTextureDescriptor descriptor;nmb_texture_describe(handle,&descriptor);view.descriptor=descriptor;
 view.parent=(id<MTLTexture>)self;view.buffer=_buffer;view.offset=_offset;view.rowBytes=_rowBytes;
 if(_surface)view.surface=(IOSurfaceRef)CFRetain(_surface);
 return (id<MTLTexture>)view;
}
- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)format {
 return [self view:nmb_texture_new_view(_handle,format,0,nullptr,nullptr,true)];
}
- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)format textureType:(MTLTextureType)type levels:(NSRange)levels slices:(NSRange)slices {
 const uint64_t l[2]={levels.location,levels.length},s[2]={slices.location,slices.length};
 return [self view:nmb_texture_new_view(_handle,format,type,l,s,false)];
}
- (void)replaceRegion:(MTLRegion)r mipmapLevel:(NSUInteger)level slice:(NSUInteger)slice withBytes:(const void *)bytes bytesPerRow:(NSUInteger)row bytesPerImage:(NSUInteger)image {
 const uint64_t region[6]={r.origin.x,r.origin.y,r.origin.z,r.size.width,r.size.height,r.size.depth};
 const uint64_t length=(r.size.depth>1?image*(r.size.depth-1):0)+row*r.size.height;
 if(!nmb_texture_replace(_handle,region,level,slice,bytes,row,image,length))NBLOG(@"native-metal: replaceRegion refused: %@",lastError());
}
- (void)replaceRegion:(MTLRegion)r mipmapLevel:(NSUInteger)level withBytes:(const void *)bytes bytesPerRow:(NSUInteger)row {
 [self replaceRegion:r mipmapLevel:level slice:0 withBytes:bytes bytesPerRow:row bytesPerImage:row*r.size.height];
}
- (void)getBytes:(void *)bytes bytesPerRow:(NSUInteger)row bytesPerImage:(NSUInteger)image fromRegion:(MTLRegion)r mipmapLevel:(NSUInteger)level slice:(NSUInteger)slice {
 const uint64_t region[6]={r.origin.x,r.origin.y,r.origin.z,r.size.width,r.size.height,r.size.depth};
 const uint64_t length=(r.size.depth>1?image*(r.size.depth-1):0)+row*r.size.height;
 if(!nmb_texture_get_bytes(_handle,bytes,row,image,length,region,level,slice))NBLOG(@"native-metal: getBytes refused: %@",lastError());
}
- (void)getBytes:(void *)bytes bytesPerRow:(NSUInteger)row fromRegion:(MTLRegion)r mipmapLevel:(NSUInteger)level {
 [self getBytes:bytes bytesPerRow:row bytesPerImage:row*r.size.height fromRegion:r mipmapLevel:level slice:0];
}
@end
// A command can use many independent imports/views of one IOSurface. Their
// owners keep the mapping live through this phase. Maintain each exact CPU
// range once; no address identity or decision is carried into another phase
// or command, and distinct/partially overlapping ranges retain their calls.
static void flushSurfaces(NSMutableSet *surfaces) {
 if(!surfaces.count)return;
 std::vector<std::pair<uintptr_t,size_t>> ranges;
 ranges.reserve(surfaces.count);
 for(NBTexture *texture in surfaces)if(texture.surface)
  ranges.emplace_back(reinterpret_cast<uintptr_t>(IOSurfaceGetBaseAddress(texture.surface)),IOSurfaceGetAllocSize(texture.surface));
 std::sort(ranges.begin(),ranges.end());
 const auto last=std::unique(ranges.begin(),ranges.end());
 gSurfaceFlushDuplicates+=(uint64_t)(ranges.end()-last);
 for(auto it=ranges.begin();it!=last;++it) {
  const uint64_t began=nowUs();
  sys_dcache_flush(reinterpret_cast<void *>(it->first),it->second);
  gSurfaceFlushBytes+=it->second;gTimeSurfaceFlush.add(began);
 }
}
static NBTexture *textureOnBuffer(NBBuffer *buffer,MTLTextureDescriptor *d,NSUInteger offset,NSUInteger row,bool surface) {
 NmbTextureDescriptor descriptor;if(!buffer || !textureDescriptor(d,&descriptor))return nil;
 NmbTexture *handle=nmb_texture_new_on_buffer(buffer.handle,&descriptor,offset,row,surface);
 if(!handle){NBLOG(@"native-metal: texture over buffer refused: %@",lastError());return nil;}
 NBTexture *texture=[NBTexture new];texture.handle=handle;nmb_texture_describe(handle,&descriptor);texture.descriptor=descriptor;
 texture.buffer=buffer;texture.offset=offset;texture.rowBytes=row;return texture;
}
static uint64_t sweetTexelBytes(MTLPixelFormat f);
@implementation NBBuffer (Textures)
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)offset bytesPerRow:(NSUInteger)row {
 // 0107 (B29, diagnostic): 1-byte texels or rows not 64-byte aligned, the layouts Turnip refuses. Off: M2V_B29_LOG=0.
 if(sweetTexelBytes(d.pixelFormat)==1 || row%64)B29LOG(@"B29 bufferTexture %lux%lu fmt %lu usage %lu storage %lu offset %lu row %lu len %lu",(unsigned long)d.width,(unsigned long)d.height,(unsigned long)d.pixelFormat,(unsigned long)d.usage,(unsigned long)d.storageMode,(unsigned long)offset,(unsigned long)row,(unsigned long)self.length);
 return (id<MTLTexture>)textureOnBuffer(self,d,offset,row,false);
}
- (id<MTLTexture>)newLinearTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)offset bytesPerRow:(NSUInteger)row bytesPerImage:(NSUInteger)image {
 NmbTextureDescriptor descriptor;if(!textureDescriptor(d,&descriptor))return nil;
 NmbTexture *handle=nmb_texture_new_linear(self.handle,&descriptor,offset,row,image);
 if(!handle){NBLOG(@"native-metal: linear texture refused: %@",lastError());return nil;}
 NBTexture *texture=[NBTexture new];texture.handle=handle;nmb_texture_describe(handle,&descriptor);texture.descriptor=descriptor;
 texture.buffer=self;texture.offset=offset;texture.rowBytes=row;return (id<MTLTexture>)texture;
}
@end

// ---- libraries, functions, states ----
@interface NBLibrary : NBObject
@property(nonatomic) NmbLibrary *handle;
@end
@interface NBFunction : NBObject
@property(nonatomic) NmbFunction *handle;
@property(nonatomic,copy) NSString *name;
@property(nonatomic) MTLFunctionType functionType;
@property(nonatomic) std::string constantsText;   // name:type:hex of each constant value it was specialized with (the journal's)
@end
@implementation NBFunction
- (void)dealloc {nmb_function_release(_handle);}
- (MTLFunctionOptions)options {return MTLFunctionOptionNone;}
@end
// The target's own accessors of MTLFunctionConstantValues (as the offline frontend reads them); their ABI is checked before any call.
@interface MTLFunctionConstantValues (NBOriginalConstantAccess)
- (NSArray *)newIndexedConstantArray;
- (NSArray *)newNamedConstantArray;
@end
@protocol NBOriginalConstantValue
- (NSUInteger)dataType;
- (void *)data;
- (NSUInteger)index;
- (NSString *)name;
@end
static bool methodABI(id object,SEL selector,const char *encoding) {
 Method method=class_getInstanceMethod(object_getClass(object),selector);
 return method && !strcmp(method_getTypeEncoding(method),encoding);
}
// Bytes of a function constant of MTLDataType `type`: lanes packed tightly, as metal2vulkan takes them (its CLI: bool3 = 3 bytes).
// 3-wide vectors added for iOS 16, whose QuartzCore specialises fixed_frag with uchar3 fc_color_opcodes0/1 (MTLDataTypeUChar3).
static uint64_t constantBytes(uint64_t type) {
 switch(type) {
  case 5:case 31:case 35:return 12;	/* float3, int3, uint3 */
  case 18:case 39:case 43:return 6;	/* half3, short3, ushort3 */
  case 47:case 51:case 55:return 3;	/* char3, uchar3, bool3 */
  case 83:case 87:return 24;		/* long3, ulong3 */
  case 3:case 29:case 33:return 4;
  case 4:case 30:case 34:return 8;
  case 6:case 32:case 36:return 16;
  case 16:case 37:case 41:return 2;
  case 17:case 38:case 42:return 4;
  case 19:case 40:case 44:return 8;
  case 45:case 49:case 53:return 1;
  case 46:case 50:case 54:return 2;
  case 48:case 52:case 56:return 4;
  case 81:case 85:return 8;
  case 82:case 86:return 16;
  case 84:case 88:return 32;
  default:return 0;
 }
}
static std::string quartzCoreCallers();
@implementation NBLibrary
- (void)dealloc {nmb_library_release(_handle);}
- (id<MTLFunction>)newFunctionWithName:(NSString *)name constantValues:(MTLFunctionConstantValues *)values error:(NSError **)error {
 if(!name || !values || !methodABI(values,@selector(newIndexedConstantArray),"@16@0:8") || !methodABI(values,@selector(newNamedConstantArray),"@16@0:8")) {
  NBLOG(@"native-metal: %@: unqualified function constant values (%s)",name,values?object_getClassName(values):"nil");
  if(error)*error=nbError(@"unqualified function constant values");return nil;
 }
 MTLFunctionConstantValues *snapshot=[values copy];
 NSArray *indexed=[snapshot newIndexedConstantArray],*named=[snapshot newNamedConstantArray];
 std::vector<NmbConstant> constants;
 for(unsigned group=0;group<2;group++)for(id value in (group?named:indexed)) {
  if(!methodABI(value,@selector(dataType),"Q16@0:8") || !methodABI(value,@selector(data),"^v16@0:8") ||
     !methodABI(value,group?@selector(name):@selector(index),group?"@16@0:8":"Q16@0:8")) {
   NBLOG(@"native-metal: %@: unqualified constant value ABI (%s)",name,object_getClassName(value));
   if(error)*error=nbError(@"unqualified constant value ABI");return nil;
  }
  id<NBOriginalConstantValue> v=value;
  const uint64_t bytes=constantBytes(v.dataType);
  if(!bytes || !v.data){NBLOG(@"native-metal: %@: constant type %lu is not qualified",name,(unsigned long)v.dataType);
   if(error)*error=nbError([NSString stringWithFormat:@"constant type %lu is not qualified",(unsigned long)v.dataType]);return nil;}
  constants.push_back({v.dataType,group?0:v.index,group?v.name.UTF8String:nullptr,v.data,bytes});
 }
 std::string text;
 for(const NmbConstant &c:constants) {
  char head[32];if(!c.name)snprintf(head,sizeof head,"#%llu",(unsigned long long)c.index);
  text+=text.empty()?"":" ";text+=c.name?c.name:head;snprintf(head,sizeof head,":%llu:",(unsigned long long)c.type);text+=head;
  for(uint64_t i=0;i<c.length;i++){snprintf(head,sizeof head,"%02x",((const uint8_t *)c.bytes)[i]);text+=head;}
 }
 const uint64_t functionBegan=nowUs();
 NmbFunction *handle=nmb_function_new_specialized(_handle,name.UTF8String,constants.data(),(uint32_t)constants.size());gTimeFunction.add(functionBegan);
 // One S line per specialization (int-155: every QuartzCore pipeline was made on its draw, and a refused generic function would leave its
 // generic pipeline unmade without a word): the function, its constants, the time, the refusal and QuartzCore's callers.
 if(!gJournalPath.empty()) {
  char head[96];snprintf(head,sizeof head,"S t=%llu us=%llu ok=%d name=",(unsigned long long)((functionBegan-gJournalBegan)/1000),
      (unsigned long long)(nowUs()-functionBegan),handle?1:0);
  journal(head+std::string(name.UTF8String?:"?")+" k="+text+(handle?std::string():" err="+std::string(lastError().UTF8String?:"?"))+" | "+quartzCoreCallers());
 }
 if(!handle){
  noteProblem([NSString stringWithFormat:@"specialized function %@: %@",name,lastError()]);
  if(error)*error=nbError([NSString stringWithFormat:@"specialized function: %@",lastError()]);return nil;
 }
 NBFunction *function=[NBFunction new];function.handle=handle;function.name=name;function.functionType=(MTLFunctionType)nmb_function_type(handle);
 function.constantsText=text;if(error)*error=nil;return (id<MTLFunction>)function;
}
- (void)newFunctionWithName:(NSString *)name constantValues:(MTLFunctionConstantValues *)values completionHandler:(void (^)(id<MTLFunction>,NSError *))handler {
 NSError *error=nil;id<MTLFunction> function=[self newFunctionWithName:name constantValues:values error:&error];
 handler(function,error);
}
// 0111 (B24): Maps asks with a pipeline library; there is none here, so it is ignored.
- (id<MTLFunction>)newFunctionWithName:(NSString *)name constantValues:(MTLFunctionConstantValues *)values pipelineLibrary:(id)library error:(NSError **)error {
 (void)library;return [self newFunctionWithName:name constantValues:values error:error];
}
- (void)newFunctionWithDescriptor:(MTLFunctionDescriptor *)d completionHandler:(void (^)(id<MTLFunction>,NSError *))handler {
 NSError *error=nil;id<MTLFunction> function=[self newFunctionWithDescriptor:d error:&error];handler(function,error);
}
- (MTLLibraryType)type {return MTLLibraryTypeExecutable;}
// The descriptor's name and constants; its specialized name only labels Metal's function and its options ask for compile-time work the
// runtime does when the function is made.
- (id<MTLFunction>)newFunctionWithDescriptor:(MTLFunctionDescriptor *)d error:(NSError **)error {
 if(!d.name){if(error)*error=nbError(@"a function name is required");return nil;}
 if(d.constantValues)return [self newFunctionWithName:d.name constantValues:d.constantValues error:error];
 id<MTLFunction> function=[self newFunctionWithName:d.name];
 if(!function){if(error)*error=nbError([NSString stringWithFormat:@"function %@: %@",d.name,lastError()]);return nil;}
 if(error)*error=nil;return function;
}
- (NSArray<NSString *> *)functionNames {
 NSMutableArray *names=[NSMutableArray array];
 for(uint32_t i=0,n=nmb_library_function_count(_handle);i<n;i++) {
  char name[256];nmb_library_function_name(_handle,i,name,sizeof name);[names addObject:[NSString stringWithUTF8String:name]];
 }
 return names;
}
- (id<MTLFunction>)newFunctionWithName:(NSString *)name {
 const uint64_t functionBegan=nowUs();
 NmbFunction *handle=name?nmb_function_new(_handle,name.UTF8String):nullptr;gTimeFunction.add(functionBegan);
 if(!handle)return nil;
 NBFunction *function=[NBFunction new];function.handle=handle;function.name=name;function.functionType=(MTLFunctionType)nmb_function_type(handle);
 return (id<MTLFunction>)function;
}
@end
@interface NBComputePipeline : NBObject
@property(nonatomic) NmbComputePipeline *handle;
@end
@implementation NBComputePipeline
- (void)dealloc {nmb_compute_pipeline_release(_handle);}
- (NSUInteger)maxTotalThreadsPerThreadgroup {return nmb_compute_pipeline_max_threads(_handle);}
- (NSUInteger)threadExecutionWidth {return nmb_compute_pipeline_execution_width(_handle);}
@end
@interface NBRenderPipeline : NBObject
@property(nonatomic) NmbRenderPipeline *handle;
@property(nonatomic) std::string vertexName,fragmentName;   // sweet drawlog: what a draw's pipeline was (only filled when SWEET_DRAWLOG is set)
@property(nonatomic) BOOL blend;
@property(nonatomic) NSUInteger writeMask;
@property(nonatomic) unsigned opaqueMask,writtenMask;   // sweet cull: colour attachments written with blending off and a full write mask
@property(nonatomic) NSUInteger vstride;   // sweet cull: stride of vertex layout 1 (0: no vertex descriptor)
@property(nonatomic) BOOL vpos0;           // sweet cull: vertex descriptor has a float4 attribute at offset 0 of buffer 1
// sweet deadfill: attribute 0 (format, offset, buffer, its layout's stride), the first 4-wide colour attribute of the same buffer, raster samples, alpha to coverage
@property(nonatomic) unsigned dfPosFmt,dfPosOff,dfPosBuf,dfStride,dfColFmt,dfColOff,dfSamples;
@property(nonatomic) BOOL dfA2C;
@end
@implementation NBRenderPipeline
- (void)dealloc {nmb_render_pipeline_release(_handle);}
// The threadgroup size a tile dispatch of this pipeline could use: the device's limit (no tile shaders are offered).
- (NSUInteger)maxTotalThreadsPerThreadgroup {
 uint64_t s[3]={};if(!nmb_device_max_threads_per_threadgroup(gDevice,s))return 0;
 return s[0];
}
@end
@interface NBSampler : NBObject
@property(nonatomic) NmbSampler *handle;
@end
@implementation NBSampler
- (void)dealloc {nmb_sampler_release(_handle);}
- (NSUInteger)pixelFormat {return MTLPixelFormatInvalid;}   // SPI: a sampler built from a plain descriptor names no format
@end
@interface NBDepthStencil : NBObject
@property(nonatomic) NmbDepthStencil *handle;
@end
@implementation NBDepthStencil
- (void)dealloc {nmb_depth_stencil_release(_handle);}
@end

// ---- events and fences ----
@interface NBEvent : NBObject
@property(nonatomic) NmbEvent *handle;
@end
@implementation NBEvent
- (void)dealloc {nmb_event_release(_handle);}
@end
@interface NBSharedEvent : NBEvent
@end
static void notified(void *context,uint64_t value);
@implementation NBSharedEvent
- (uint64_t)signaledValue {return nmb_shared_event_value(self.handle);}
- (void)setSignaledValue:(uint64_t)value {nmb_shared_event_set_value(self.handle,value);}
- (BOOL)waitUntilSignaledValue:(uint64_t)value timeoutMS:(uint64_t)milliseconds {return nmb_shared_event_wait(self.handle,value,milliseconds);}
// The block runs on the listener's dispatch queue (Metal's contract); the bridge's listener thread only forwards it there.
- (void)notifyListener:(MTLSharedEventListener *)listener atValue:(uint64_t)value block:(MTLSharedEventNotificationBlock)block {
 if(!block)return;
 dispatch_queue_t queue=listener.dispatchQueue?:dispatch_get_global_queue(QOS_CLASS_DEFAULT,0);
 NSArray *job=@[self,queue,[block copy]];
 if(!nmb_shared_event_notify(self.handle,value,notified,(__bridge_retained void *)job)) {
  (void)(__bridge_transfer NSArray *)(__bridge void *)job;
  NBLOG(@"native-metal: notifyListener refused: %@",lastError());
 }
}
// A handle for another process needs a kernel-backed event this bridge does not have yet: refused, never a handle that could not work.
- (MTLSharedEventHandle *)newSharedEventHandle {
 if(gMissing)gMissing("NBSharedEvent","newSharedEventHandle (cross-process)");
 return nil;
}
@end
static void notified(void *context,uint64_t value) {
 NSArray *job=(__bridge_transfer NSArray *)context;
 id<MTLSharedEvent> event=job[0];MTLSharedEventNotificationBlock block=job[2];
 dispatch_async(job[1],^{block(event,value);});
}
@interface NBFence : NBObject
@property(nonatomic) NmbFence *handle;
@end
@implementation NBFence
- (void)dealloc {nmb_fence_release(_handle);}
@end

// ---- command buffers and encoders ----
@interface NBQueue : NBObject
@property(nonatomic) NmbQueue *handle;
@property(atomic,strong) dispatch_queue_t completionQueue;   // SPI: where command buffers' handlers run (nil: the runtime's thread)
@end
// sweet ctl (SWEET_CTL=/path): per command buffer mach_absolute_time stamps of the commit -> completion chain, to split the
// GPU end -> SwapEnd gap. Off (one cached getenv) unless set. Line: "CTL pid idx C D F0 F1 Q H" (hex mach ticks).
static const char *ctlPath(){
 static const char *path=[]{const char *p=getenv("SWEET_CTL");return p&&*p?p:(const char *)nullptr;}();
 return path;
}
struct CtlRec{uint32_t idx;uint64_t c,d,f0,f1,q,h;};
static CtlRec gCtlBuf[512];
static int gCtlN=0;
static uint64_t gCtlLastFlush=0;
static std::mutex gCtlLock;
static void ctlFlushLocked(){
 if(!gCtlN)return;
 std::string out;char line[160];const int pid=(int)getpid();
 for(int i=0;i<gCtlN;i++){
  const CtlRec &r=gCtlBuf[i];
  int n=snprintf(line,sizeof line,"CTL %d %x %llx %llx %llx %llx %llx %llx\n",pid,r.idx,(unsigned long long)r.c,(unsigned long long)r.d,
   (unsigned long long)r.f0,(unsigned long long)r.f1,(unsigned long long)r.q,(unsigned long long)r.h);
  out.append(line,n);
 }
 gCtlN=0;
 int fd=open(ctlPath(),O_WRONLY|O_CREAT|O_APPEND,0666);
 if(fd>=0){(void)!write(fd,out.data(),out.size());close(fd);}
}
static void ctlPush(const CtlRec &r){
 std::lock_guard<std::mutex> lock(gCtlLock);
 gCtlBuf[gCtlN++]=r;
 const uint64_t now=mach_absolute_time();
 if(gCtlN==512||now-gCtlLastFlush>2*19200000ull){gCtlLastFlush=now;ctlFlushLocked();}
}
@interface NBCommandBuffer : NBObject {
@public
 uint64_t ctlC,ctlD,ctlF0,ctlF1,ctlQ;   // sweet ctl stamps
 EncodedWork encodedWork;   // the command buffer's single encoder thread, not global counter deltas
 uint64_t createdUs;
}
@property(nonatomic) NmbCommandBuffer *handle;
@property(nonatomic,strong) NBQueue *queue;
@property(atomic,copy) NSString *encodeFailure;
@property(atomic) BOOL committed,failedLocally;
@property(nonatomic) uint64_t committedUs;
@property(nonatomic) uint32_t commitIndex;
@property(nonatomic,strong) NSMutableArray *scheduledHandlers,*completedHandlers;
@property(nonatomic,strong) NSMutableSet *surfaces;
// Runtime handles retain GPU storage, but only the Objective-C owner retains
// a no-copy deallocator, its memory owner and texture/heap ancestry.
@property(nonatomic,strong) NSMutableSet *resourceOwners;
@property(nonatomic,strong) dispatch_group_t handlers;
@property(nonatomic,strong) dispatch_group_t schedulingHandlers;
@property(nonatomic) BOOL schedulingDelivered;
- (void)refuse:(NSString *)what;
@end
@interface NBEncoder : NBObject
@property(nonatomic) NmbEncoder *handle;
@property(nonatomic,strong) NBCommandBuffer *commandBuffer;
@end
static void waitForResource(id resource) {
 const uint64_t began=nowUs();
 NSArray *pending;@synchronized(gInFlight){pending=gInFlight.allObjects;}
 for(NBCommandBuffer *commandBuffer in pending) {
  int dependency=-1;
  if([resource isKindOfClass:[NBBuffer class]])dependency=nmb_command_buffer_uses_buffer(commandBuffer.handle,[(NBBuffer *)resource handle]);
  else if([resource isKindOfClass:[NBTexture class]])dependency=nmb_command_buffer_uses_texture(commandBuffer.handle,[(NBTexture *)resource handle]);
  gResourceChecks++;
  // An unknown result retains the old conservative wait. False means no matching storage
  // (or GPU work already completed), not a fabricated completion for a pending dependency.
  if(dependency!=0)[(id<MTLCommandBuffer>)commandBuffer waitUntilCompleted];else gResourceSkipped++;
 }
 gTimeResourceWait.add(began);
}
@implementation NBEncoder
- (void)dealloc {if(_handle)nmb_encoder_discard(_handle);}
- (bool)check:(bool)ok what:(const char *)what {
 if(!ok)[_commandBuffer refuse:[NSString stringWithFormat:@"%s: %@",what,lastError()]];
 return ok;
}
- (void)endEncoding {
 NmbEncoder *handle=_handle;_handle=nullptr;
 if(!handle){[_commandBuffer refuse:@"endEncoding twice"];return;}
 [self check:nmb_encoder_end(handle) what:"endEncoding"];
}
- (void)insertDebugSignpost:(NSString *)s {(void)s;}
- (void)pushDebugGroup:(NSString *)s {(void)s;}
- (void)popDebugGroup {}
// Stage 0 compute, 1 vertex, 2 fragment (the bridge's numbering).
- (void)buffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset index:(NSUInteger)index stage:(uint32_t)stage {
 if(buffer && ![(id)buffer isKindOfClass:[NBBuffer class]]){[_commandBuffer refuse:@"foreign buffer"];return;}
 if(buffer)[_commandBuffer.resourceOwners addObject:buffer];
 [self check:nmb_encoder_set_buffer(_handle,stage,buffer?[(NBBuffer *)buffer handle]:nullptr,offset,(uint32_t)index) what:"setBuffer"];
}
- (void)texture:(id<MTLTexture>)texture index:(NSUInteger)index stage:(uint32_t)stage {
 if(texture && ![(id)texture isKindOfClass:[NBTexture class]]){[_commandBuffer refuse:@"foreign texture"];return;}
 if(texture)[_commandBuffer.resourceOwners addObject:texture];
 if(texture && [(NBTexture *)texture surface])[_commandBuffer.surfaces addObject:texture];
 if(texture && [(NBTexture *)texture planarSurface])planarFill((NBTexture *)texture); // 0112: refresh the converted copy
 [self check:nmb_encoder_set_texture(_handle,stage,texture?[(NBTexture *)texture handle]:nullptr,(uint32_t)index) what:"setTexture"];
}
- (void)sampler:(id<MTLSamplerState>)sampler index:(NSUInteger)index stage:(uint32_t)stage {
 if(sampler && ![(id)sampler isKindOfClass:[NBSampler class]]){[_commandBuffer refuse:@"foreign sampler"];return;}
 [self check:nmb_encoder_set_sampler(_handle,stage,sampler?[(NBSampler *)sampler handle]:nullptr,(uint32_t)index) what:"setSamplerState"];
}
- (void)bytes:(const void *)bytes length:(NSUInteger)length index:(NSUInteger)index stage:(uint32_t)stage {
 [self check:nmb_encoder_set_bytes(_handle,stage,bytes,length,(uint32_t)index) what:"setBytes"];
}
- (void)fence:(id<MTLFence>)fence stages:(uint64_t)stages update:(bool)update {
 if(![(id)fence isKindOfClass:[NBFence class]]){[_commandBuffer refuse:@"foreign fence"];return;}
 NmbFence *handle=[(NBFence *)fence handle];
 [self check:update?nmb_encoder_update_fence(_handle,handle,stages):nmb_encoder_wait_fence(_handle,handle,stages) what:update?"updateFence":"waitForFence"];
}
- (void)updateFence:(id<MTLFence>)fence {[self fence:fence stages:0 update:true];}
- (void)waitForFence:(id<MTLFence>)fence {[self fence:fence stages:0 update:false];}
- (void)offset:(NSUInteger)offset index:(NSUInteger)index stage:(uint32_t)stage {
 [self check:nmb_encoder_set_buffer_offset(_handle,stage,offset,(uint32_t)index) what:"setBufferOffset"];
}
@end

@interface NBComputeEncoder : NBEncoder
@end
@implementation NBComputeEncoder
- (MTLDispatchType)dispatchType {return MTLDispatchTypeSerial;}
- (void)setComputePipelineState:(id<MTLComputePipelineState>)state {
 if(![(id)state isKindOfClass:[NBComputePipeline class]]){[self.commandBuffer refuse:@"foreign compute pipeline"];return;}
 [self check:nmb_compute_set_pipeline(self.handle,[(NBComputePipeline *)state handle]) what:"setComputePipelineState"];
}
- (void)setBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i {[self buffer:b offset:o index:i stage:0];}
- (void)setBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i {[self offset:o index:i stage:0];}
- (void)setBuffers:(const id<MTLBuffer> __unsafe_unretained [])b offsets:(const NSUInteger [])o withRange:(NSRange)r {
 for(NSUInteger k=0;k<r.length;k++)[self buffer:b[k] offset:o[k] index:r.location+k stage:0];
}
- (void)setBytes:(const void *)p length:(NSUInteger)n atIndex:(NSUInteger)i {[self bytes:p length:n index:i stage:0];}
- (void)setTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i {[self texture:t index:i stage:0];}
- (void)setTextures:(const id<MTLTexture> __unsafe_unretained [])t withRange:(NSRange)r {
 for(NSUInteger k=0;k<r.length;k++)[self texture:t[k] index:r.location+k stage:0];
}
- (void)setSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i {[self sampler:s index:i stage:0];}
- (void)setThreadgroupMemoryLength:(NSUInteger)n atIndex:(NSUInteger)i {
 [self check:nmb_compute_set_threadgroup_memory(self.handle,(uint32_t)n,(uint32_t)i) what:"setThreadgroupMemoryLength"];
}
- (void)dispatch:(MTLSize)size group:(MTLSize)group threads:(bool)threads {
 gDispatches++;self.commandBuffer->encodedWork.dispatches++;
 const uint64_t s[3]={size.width,size.height,size.depth},g[3]={group.width,group.height,group.depth};
 [self check:nmb_compute_dispatch(self.handle,s,g,threads) what:"dispatch"];
}
- (void)dispatchThreads:(MTLSize)t threadsPerThreadgroup:(MTLSize)g {[self dispatch:t group:g threads:true];}
- (void)dispatchThreadgroups:(MTLSize)t threadsPerThreadgroup:(MTLSize)g {[self dispatch:t group:g threads:false];}
- (void)useResource:(id<MTLResource>)r usage:(MTLResourceUsage)u {(void)r;(void)u;}
- (void)memoryBarrierWithScope:(MTLBarrierScope)s {(void)s;}
@end

// sweet drawlog (SWEET_DRAWLOG=/path): a text dump of what the big Core Animation passes draw, to see why they are slow. Off (one cached getenv)
// unless set; only passes with a colour target >= 1000x2000 are recorded, and only the first 20 such passes with >= 10 draws are written.
static const char *drawLogPath() {
 static const char *path=[]{const char *p=getenv("SWEET_DRAWLOG");return p&&*p?p:(const char *)nullptr;}();
 return path;
}
static std::atomic<int> gDrawLogBlocks;
static void dlFloats(std::string &out,const void *p,size_t bytes,unsigned maxFloats) {
 char t[32];const float *f=(const float *)p;size_t n=bytes/4;if(n>maxFloats)n=maxFloats;
 for(size_t i=0;i<n;i++){float v;memcpy(&v,f+i,4);snprintf(t,sizeof t,"%s%g",i?",":"",v);out+=t;}
}
struct DrawLog {
 unsigned long w=0,h=0,format=0,load=0,store=0;bool depth=false;
 std::string text;unsigned draws=0;
 std::string pipeline="-";bool blend=false;unsigned long mask=0;
 struct VB {__unsafe_unretained NBBuffer *b=nil;unsigned long offset=0;} vbs[31];
 struct FT {__unsafe_unretained NBTexture *t=nil;bool sampler=false;} fts[16];
 bool viewport=false,scissor=false;double vp[6]={};unsigned long sc[4]={};
 std::string vbytes,fbytes;   // setVertexBytes / setFragmentBytes calls since the previous draw
 void bytes(std::string &out,const void *p,NSUInteger n,NSUInteger i,unsigned maxFloats) {
  char t[48];snprintf(t,sizeof t," [%lu len=%lu: ",(unsigned long)i,(unsigned long)n);out+=t;dlFloats(out,p,n,maxFloats);out+="]";
 }
 void draw(unsigned long prim,unsigned long start,unsigned long count,unsigned long inst,bool indexed) {
  char t[160];
  snprintf(t,sizeof t,"draw %u prim=%lu %s=%lu/%lu inst=%lu blend=%d mask=%lu pipe=",draws,prim,indexed?"idx":"vtx",start,count,inst,(int)blend,mask);
  text+=t;text+=pipeline;
  if(viewport){snprintf(t,sizeof t," vp=%g,%g,%g,%g",vp[0],vp[1],vp[2],vp[3]);text+=t;}
  if(scissor){snprintf(t,sizeof t," sc=%lu,%lu,%lu,%lu",sc[0],sc[1],sc[2],sc[3]);text+=t;}
  text+="\n";
  if(!vbytes.empty()){text+="  vbytes:"+vbytes+"\n";vbytes.clear();}
  for(unsigned i=0;i<31;i++)if(vbs[i].b) {
   NBBuffer *b=vbs[i].b;snprintf(t,sizeof t,"  vbuf %u off=%lu len=%lu",i,vbs[i].offset,(unsigned long)b.length);text+=t;
   void *c=b.storageMode==MTLStorageModeShared?b.contents:nullptr;
   if(c && vbs[i].offset<b.length){text+=" f=";dlFloats(text,(const char *)c+vbs[i].offset,b.length-vbs[i].offset,16);}
   text+="\n";
  }
  for(unsigned i=0;i<16;i++)if(fts[i].t||fts[i].sampler) {
   if(fts[i].t){NBTexture *x=fts[i].t;snprintf(t,sizeof t,"  ftex %u %lux%lu fmt=%lu mips=%lu",i,(unsigned long)x.width,(unsigned long)x.height,(unsigned long)x.pixelFormat,(unsigned long)x.mipmapLevelCount);}
   else snprintf(t,sizeof t,"  ftex %u none",i);
   text+=t;if(fts[i].sampler)text+=" sampler";text+="\n";
  }
  if(!fbytes.empty()){text+="  fbytes:"+fbytes+"\n";fbytes.clear();}
  draws++;
 }
 void finish() {
  if(w<1000||h<2000||draws<10||gDrawLogBlocks>=20)return;
  gDrawLogBlocks++;
  if(FILE *f=fopen(drawLogPath(),"a")) {
   fprintf(f,"=== pass %lux%lu fmt=%lu load=%lu store=%lu depthstencil=%d draws=%u\n%s",w,h,format,load,store,(int)depth,draws,text.c_str());
   fclose(f);
  }
 }
};
// sweet cull (SWEET_CULL=1): dead-draw culling. Off (one cached getenv) unless set. A render pass with one colour target and no depth/stencil defers
// every encoder call; at endEncoding, draws before the last full-target opaque rectangle draw (whitelisted Core Animation pipelines) are skipped.
static bool cullEnabled() {
 static const bool on=[]{const char *p=getenv("SWEET_CULL");return p&&atoi(p)!=0;}();
 return on;
}
static std::atomic<unsigned> gCullPasses,gCullDraws;
// sweet cull: why a whitelisted draw was not covering (first 12 times per process)
static void cullWhy(int why,NSString *l,const char *extra="") {
 static std::atomic<int> said{0},img{0};if([l hasSuffix:@"Isrc"]?img++>=12:said++>=4)return;   // the wallpaper quad gets its own 12
 char t[300];snprintf(t,sizeof t,"SWEET CULL: not covering #%d %s %s",why,l.UTF8String,extra);sm2_logs(t);
}
// sweet deadfill (SWEET_DEADFILL, from ios-redfin's frontend, MIT): 0 off (default), 1 drop Core Animation's full-target opaque
// constant-colour fills (draw.PBGRAXm_A2Xhf) whose next draw is the same fill (same pipeline, same other vertex bindings: the same
// pixels), 2 also: the creation of a pass loading colour 0 waits for endEncoding, and if its first surviving draw is such a fill whose
// transform maps it onto the whole target, the pass clears to its colour (8-bit code) and the draw goes. backboardd's passes only:
// colour 0 an 8-bit unorm single-sample 2D IOSurface, no depth/stencil/visibility; colours 1..7 may be attached (a clear is refused when
// the fill writes one that is not memoryless). Uses sweet cull's deferral; the telemetry's "DF" row counts drops and clears.
static int deadfillMode() {
 static const int mode=[]{const char *p=getenv("SWEET_DEADFILL");if(!p||!*p||strcmp(getprogname(),"backboardd"))return 0;return std::max(0,std::min(2,atoi(p)));}();
 return mode;
}
static std::atomic<uint64_t> gDfDropped,gDfCleared,gDfPasses;
struct CullEntry {bool fill=false,clearOK=false;float col[4]={};uint64_t bind=0;__unsafe_unretained NBRenderPipeline *fpipe=nil;   // sweet deadfill
 void (^blk)(void)=nil;bool draw=false,covering=false;unsigned writes=0xff,over=0;long cx0=0,cy0=0,cx1=0,cy1=0;NSString *label=nil;};   // c*: the draw's clip (scissor within the target)   // writes: targets the draw may change; over: targets a covering draw overwrites
@interface NBRenderEncoder : NBEncoder
- (void)drawLogStart:(MTLRenderPassDescriptor *)d;
- (bool)dfHold:(const NmbRenderPassDescriptor &)pass load:(MTLLoadAction)load;
@end
@implementation NBRenderEncoder {
 std::unique_ptr<DrawLog> _dl;   // sweet drawlog: null unless this pass is a big one and SWEET_DRAWLOG is set
 // sweet cull
 BOOL _defer,_noCull,_vpSet,_scSet;
 unsigned long _tw,_th;unsigned _attMask;
 std::vector<CullEntry> _ops;
 NBRenderPipeline *_pipe;
 NBBuffer *_cvb;NSUInteger _cvbOff;
 double _cvp[6];unsigned long _csc[4];
 NSUInteger _cvis;
 // sweet deadfill: vertex-stage bindings 0..7 (buffer + offset, or bytes), a depth/stencil state set, the held pass (mode 2), colours 1..7 not memoryless
 BOOL _dfOn,_dfDss,_dfPending,_cullPass;NBBuffer *_dfVb[8];NSUInteger _dfVbOff[8];std::vector<uint8_t> _dfVbytes[8];
 NmbRenderPassDescriptor _dfPass;unsigned _dfAttached;
}
- (void)rec:(void (^)(void))b {CullEntry e;e.blk=[b copy];_ops.push_back(e);}
- (void)clip:(long *)c {   // sweet cull: scissor within the target
 c[0]=0;c[1]=0;c[2]=(long)_tw;c[3]=(long)_th;
 if(_scSet){c[0]=std::max(c[0],(long)_csc[0]);c[1]=std::max(c[1],(long)_csc[1]);c[2]=std::min(c[2],(long)(_csc[0]+_csc[2]));c[3]=std::min(c[3],(long)(_csc[1]+_csc[3]));}
}
- (void)recDraw:(void (^)(void))b covering:(bool)c {CullEntry e;e.blk=[b copy];e.draw=true;e.covering=c;
 {long r[4];[self clip:r];e.cx0=r[0];e.cy0=r[1];e.cx1=r[2];e.cy1=r[3];}
 e.label=_pipe.label;
 if(_pipe){e.writes=_pipe.writtenMask&_attMask;e.over=_pipe.opaqueMask&_attMask;}_ops.push_back(e);}
// sweet cull: is this draw an opaque, exactly-rectangular, full-target draw of a whitelisted pipeline?
- (bool)coversPrim:(unsigned long)prim start:(NSUInteger)s count:(NSUInteger)n inst:(NSUInteger)inst indexed:(bool)indexed indexType:(MTLIndexType)ik
         indexBuffer:(NBBuffer *)ib indexOffset:(NSUInteger)io baseVertex:(NSInteger)bv {
 if(!_defer||_noCull||!_cullPass||!_pipe||prim!=MTLPrimitiveTypeTriangle||inst!=1||n!=6||_cvis)return false;
 NSString *l=_pipe.label;
 if(!l||!([l isEqualToString:@"com.apple.coreanimation.draw.PBGRAXm_A2Xhf"]||[l isEqualToString:@"com.apple.coreanimation.draw.PBGRAXm_TimgA2Xhfn_Isrc"]))return false;
 if(!(_pipe.opaqueMask&1)){char t[64];snprintf(t,sizeof t,"opaque %x att %x",_pipe.opaqueMask,_attMask);cullWhy(1,l,t);return false;}   // overwrites target 0 at least; per-draw targets are checked at replay
 long cr[4];[self clip:cr];if(cr[0]>=cr[2]||cr[1]>=cr[3])return false;
 if(_vpSet&&!(_cvp[0]<=cr[0]&&_cvp[1]<=cr[1]&&_cvp[0]+_cvp[2]>=cr[2]&&_cvp[1]+_cvp[3]>=cr[3])){cullWhy(2,l);return false;}
 if(!_cvb||_cvb.storageMode!=MTLStorageModeShared){cullWhy(4,l);return false;}
 const char *vc=(const char *)_cvb.contents;if(!vc){cullWhy(5,l);return false;}
 NSUInteger stride=48;   // assumed (drawlog); the vertex descriptor, when present, must agree on a float4 position at offset 0
 if(_pipe.vstride){if(!_pipe.vpos0){char t[64];snprintf(t,sizeof t,"vstride %lu",(unsigned long)_pipe.vstride);cullWhy(6,l,t);return false;}stride=_pipe.vstride;}
 if(stride<16){cullWhy(7,l);return false;}
 long vid[6];
 for(unsigned k=0;k<6;k++) {
  if(!indexed)vid[k]=(long)(s+k);
  else {
   if(!ib||ib.storageMode!=MTLStorageModeShared||!ib.contents){cullWhy(8,l);return false;}
   const NSUInteger isz=ik==MTLIndexTypeUInt16?2:4;
   if(io+(k+1)*isz>ib.length){cullWhy(9,l);return false;}
   uint32_t ix;const char *ip=(const char *)ib.contents+io+k*isz;
   if(isz==2){uint16_t t;memcpy(&t,ip,2);ix=t;}else memcpy(&ix,ip,4);
   vid[k]=(long)ix+(long)bv;
  }
 }
 float p[6][2];
 for(unsigned k=0;k<6;k++) {
  if(vid[k]<0){cullWhy(10,l);return false;}
  const NSUInteger off=_cvbOff+(NSUInteger)vid[k]*stride;
  if(off+16>_cvb.length){cullWhy(11,l);return false;}
  float v[4];memcpy(v,vc+off,16);
  if(!std::isfinite(v[0])||!std::isfinite(v[1])||v[3]!=1.0f){cullWhy(12,l);return false;}   // pixel space: w == 1
  p[k][0]=v[0];p[k][1]=v[1];
 }
 float x0=p[0][0],x1=x0,y0=p[0][1],y1=y0;
 for(unsigned k=1;k<6;k++){x0=std::min(x0,p[k][0]);x1=std::max(x1,p[k][0]);y0=std::min(y0,p[k][1]);y1=std::max(y1,p[k][1]);}
 if(!(x0<x1&&y0<y1)){cullWhy(13,l);return false;}
 unsigned id[6];
 for(unsigned k=0;k<6;k++) {
  if(!(p[k][0]==x0||p[k][0]==x1)||!(p[k][1]==y0||p[k][1]==y1)){cullWhy(14,l);return false;}
  id[k]=(p[k][0]==x1?1u:0u)|(p[k][1]==y1?2u:0u);
 }
 // two triangles, each three distinct corners, sharing a diagonal: together exactly the rectangle
 if(id[0]==id[1]||id[0]==id[2]||id[1]==id[2]||id[3]==id[4]||id[3]==id[5]||id[4]==id[5]){cullWhy(15,l);return false;}
 unsigned ma=(1u<<id[0])|(1u<<id[1])|(1u<<id[2]),mb=(1u<<id[3])|(1u<<id[4])|(1u<<id[5]);
 if((ma|mb)!=15||__builtin_popcount(ma&mb)!=2){cullWhy(16,l);return false;}
 unsigned sh=ma&mb,a=(unsigned)__builtin_ctz(sh),b=(unsigned)(31-__builtin_clz(sh));
 if((a^b)!=3){cullWhy(17,l);return false;}
 if(!(x0<=(float)cr[0]&&y0<=(float)cr[1]&&x1>=(float)cr[2]&&y1>=(float)cr[3])){char t[160];snprintf(t,sizeof t,"rect %.1f,%.1f..%.1f,%.1f",x0,y0,x1,y1);cullWhy(99,l,t);return false;}
 return true;
}
// sweet deadfill: vertex binding k's bytes from its offset (set bytes, else a shared buffer's contents)
- (const uint8_t *)dfData:(unsigned)k length:(size_t *)len {
 if(k>=8)return nullptr;
 if(!_dfVbytes[k].empty()){*len=_dfVbytes[k].size();return _dfVbytes[k].data();}
 NBBuffer *b=_dfVb[k];if(!b||b.storageMode!=MTLStorageModeShared||!b.contents||_dfVbOff[k]>=b.length)return nullptr;
 *len=b.length-_dfVbOff[k];return (const uint8_t *)b.contents+_dfVbOff[k];
}
// sweet deadfill: the transform (the first vertex binding other than the position's with 16 floats, column-major 4x4)
- (const float *)dfTransform:(unsigned)pos {
 for(unsigned k=0;k<8;k++) {
  if(k==pos||(_dfVbytes[k].empty()&&!_dfVb[k]))continue;
  size_t l=0;const uint8_t *c=[self dfData:k length:&l];
  return c&&l>=64&&!((uintptr_t)c&3)?(const float *)c:nullptr;
 }
 return nullptr;
}
// sweet deadfill: the vertex bindings other than the position's (contents, first 256 bytes: CA's uniforms sit in a ring buffer)
- (uint64_t)dfBindHash:(unsigned)pos {
 uint64_t h=1469598103934665603ull;
 auto mix=[&](const void *p,size_t n){const uint8_t *b=(const uint8_t *)p;for(size_t i=0;i<n;i++){h^=b[i];h*=1099511628211ull;}};
 for(unsigned k=0;k<8;k++) {
  if(k==pos||(_dfVbytes[k].empty()&&!_dfVb[k]))continue;
  mix(&k,sizeof k);size_t l=0;
  if(const uint8_t *c=[self dfData:k length:&l])mix(c,std::min<size_t>(l,256));else{const void *o=(__bridge const void *)_dfVb[k];mix(&o,sizeof o);}
 }
 return h;
}
// sweet deadfill: marks the draw just recorded when it is a full-target opaque constant-colour fill (and whether it may be the clear)
- (void)dfNote:(MTLPrimitiveType)prim start:(NSUInteger)start count:(NSUInteger)count inst:(NSUInteger)inst indexBuffer:(NBBuffer *)ib type:(MTLIndexType)itype
        offset:(NSUInteger)ioff base:(NSInteger)base {
 NBRenderPipeline *p=_pipe;CullEntry &e=_ops.back();
 if(!p||_dfDss||_cvis||inst!=1||!(prim==MTLPrimitiveTypeTriangle?count==6:prim==MTLPrimitiveTypeTriangleStrip&&count==4))return;
 if(![p.label isEqualToString:@"com.apple.coreanimation.draw.PBGRAXm_A2Xhf"]||!(p.opaqueMask&1)||p.dfSamples>1||p.dfA2C)return;
 NSNumber *cm=objc_getAssociatedObject(self,"cull"),*fm=objc_getAssociatedObject(self,"fill");
 if(cm.unsignedLongLongValue!=MTLCullModeNone||fm.unsignedLongLongValue!=MTLTriangleFillModeFill)return;   // every pixel, once
 if((p.dfPosFmt!=MTLVertexFormatFloat4&&p.dfPosFmt!=MTLVertexFormatFloat2)||!p.dfColFmt)return;
 if(_vpSet&&(_cvp[0]!=0||_cvp[1]!=0||_cvp[2]!=_tw||_cvp[3]!=_th))return;
 if(_scSet&&(_csc[0]!=0||_csc[1]!=0||_csc[2]!=_tw||_csc[3]!=_th))return;
 size_t len=0;const uint8_t *src=[self dfData:p.dfPosBuf length:&len];if(!src)return;
 const uint8_t *ix=nullptr;size_t ilen=0;
 if(ib){if(ib.storageMode!=MTLStorageModeShared||!ib.contents||ioff>=ib.length)return;ix=(const uint8_t *)ib.contents+ioff;ilen=ib.length-ioff;}
 const unsigned comps=p.dfPosFmt==MTLVertexFormatFloat4?4:2,stride=p.dfStride?p.dfStride:comps*4;
 float x0=1e30f,y0=1e30f,x1=-1e30f,y1=-1e30f,c0[4]={};float vx[6],vy[6],vz[6];
 for(unsigned k=0;k<count;k++) {
  int64_t v;
  if(ib){if(itype==MTLIndexTypeUInt16){if((k+1)*2>ilen)return;uint16_t t;memcpy(&t,ix+k*2,2);v=t;}else{if((k+1)*4>ilen)return;uint32_t t;memcpy(&t,ix+k*4,4);v=t;}v+=base;}
  else v=(int64_t)start+k;
  if(v<0)return;
  const size_t o=(size_t)v*stride+p.dfPosOff;if(o+comps*4>len)return;
  float q[4]={0,0,0,1};memcpy(q,src+o,comps*4);
  if(q[3]!=1)return;
  vx[k]=q[0];vy[k]=q[1];vz[k]=q[2];
  x0=std::min(x0,q[0]);x1=std::max(x1,q[0]);y0=std::min(y0,q[1]);y1=std::max(y1,q[1]);
  const size_t co=(size_t)v*stride+p.dfColOff;float c[4];
  if(p.dfColFmt==MTLVertexFormatHalf4){if(co+8>len)return;__fp16 h[4];memcpy(h,src+co,8);for(int i=0;i<4;i++)c[i]=(float)h[i];}
  else if(p.dfColFmt==MTLVertexFormatFloat4){if(co+16>len)return;memcpy(c,src+co,16);}
  else{if(co+4>len)return;for(int i=0;i<4;i++)c[i]=src[co+i]/255.f;}
  if(k==0)memcpy(c0,c,sizeof c0);else if(memcmp(c0,c,sizeof c0))return;
 }
 if(c0[3]!=1)return;
 const bool pixels=fabsf(x0)<=0.01f&&fabsf(y0)<=0.01f&&fabsf(x1-(float)_tw)<=0.01f&&fabsf(y1-(float)_th)<=0.01f;
 const bool clip=fabsf(x0+1)<=1e-4f&&fabsf(y0+1)<=1e-4f&&fabsf(x1-1)<=1e-4f&&fabsf(y1-1)<=1e-4f;
 if(!pixels&&!clip)return;
 // two triangles, each three distinct corners, sharing a diagonal (as sweet cull's test)
 int cr[6];
 for(unsigned k=0;k<count;k++){if(!(vx[k]==x0||vx[k]==x1)||!(vy[k]==y0||vy[k]==y1))return;cr[k]=(vx[k]==x1?1:0)|(vy[k]==y1?2:0);}
 const bool list=prim==MTLPrimitiveTypeTriangle;
 const int t0[3]={cr[0],cr[1],cr[2]},t1[3]={list?cr[3]:cr[1],list?cr[4]:cr[2],list?cr[5]:cr[3]};
 auto distinct=[](const int *t){return t[0]!=t[1]&&t[1]!=t[2]&&t[0]!=t[2];};
 if(!distinct(t0)||!distinct(t1))return;
 unsigned ma=(1u<<t0[0])|(1u<<t0[1])|(1u<<t0[2]),mb=(1u<<t1[0])|(1u<<t1[1])|(1u<<t1[2]),sh=ma&mb;
 if((ma|mb)!=15||__builtin_popcount(sh)!=2||(__builtin_ctz(sh)^(31-__builtin_clz(sh)))!=3)return;
 e.fill=true;memcpy(e.col,c0,sizeof c0);e.fpipe=p;e.bind=[self dfBindHash:p.dfPosBuf]^(clip?0x9e3779b97f4a7c15ull:0);
 // may it be the pass's clear? CA's fixed vertex function, no lasting colour 1..7 written, and the transform taking every vertex onto
 // a corner of clip space (w 1, z in [0,1]): the whole target whatever the positions' space
 if(p.vertexName.compare(0,18,"fixed_vert_lph_spc")||(p.writtenMask&_dfAttached))return;
 const float *m=[self dfTransform:p.dfPosBuf];if(!m)return;
 float cx0=1e30f,cx1=-1e30f,cy0=1e30f,cy1=-1e30f;
 for(unsigned k=0;k<count;k++) {
  const float x=vx[k],y=vy[k],z=vz[k];
  const float cx=m[0]*x+m[4]*y+m[8]*z+m[12],cy=m[1]*x+m[5]*y+m[9]*z+m[13],cz=m[2]*x+m[6]*y+m[10]*z+m[14],cw=m[3]*x+m[7]*y+m[11]*z+m[15];
  if(fabsf(cw-1)>1e-5f||fabsf(fabsf(cx)-1)>1e-4f||fabsf(fabsf(cy)-1)>1e-4f||cz<0||cz>1)return;
  cx0=std::min(cx0,cx);cx1=std::max(cx1,cx);cy0=std::min(cy0,cy);cy1=std::max(cy1,cy);
 }
 e.clearOK=cx0<=-0.999f&&cx1>=0.999f&&cy0<=-0.999f&&cy1>=0.999f;
}
// sweet deadfill mode 2: hold the creation of a deferred pass that loads colour 0 (made in dfResolve)
- (bool)dfHold:(const NmbRenderPassDescriptor &)pass load:(MTLLoadAction)load {
 if(!_dfOn||deadfillMode()<2||(load!=MTLLoadActionLoad&&load!=MTLLoadActionDontCare))return false;
 _dfPass=pass;_dfPending=YES;gDfPasses++;
 return true;
}
// sweet deadfill: a fill whose next surviving draw is the same fill is dropped; mode 2: the first surviving draw, a fill covering the
// target, becomes the held pass's clear. Then the held pass is made.
- (void)dfResolve:(const std::vector<CullEntry> &)ops dead:(std::vector<char> &)dead {
 bool clear=false;float col[4]={};
 if(!_noCull) {
  long prev=-1;
  for(long i=0;i<(long)ops.size();i++) {
   if(!ops[i].draw||dead[i])continue;
   if(prev>=0&&ops[prev].fill&&ops[i].fill&&ops[prev].fpipe==ops[i].fpipe&&ops[prev].bind==ops[i].bind){dead[prev]=1;gDfDropped++;}
   prev=i;
  }
  for(long i=0;i<(long)ops.size()&&_dfPending;i++) {
   if(!ops[i].draw||dead[i])continue;
   if(ops[i].fill&&ops[i].clearOK){dead[i]=1;clear=true;memcpy(col,ops[i].col,sizeof col);gDfCleared++;}
   break;
  }
 }
 if(!_dfPending)return;
 _dfPending=NO;
 NmbRenderPassDescriptor pass=_dfPass;
 if(clear){pass.colors[0].load=MTLLoadActionClear;for(int i=0;i<4;i++)pass.colors[0].clear[i]=std::min(255.,std::max(0.,floor(col[i]*255.+0.5)))/255.;}   // the 8-bit code
 NBCommandBuffer *cb=self.commandBuffer;
 NmbEncoder *h=cb.committed?nullptr:nmb_render_encoder_new(cb.handle,&pass);
 if(!h){[cb refuse:[NSString stringWithFormat:@"renderCommandEncoder: %@",lastError()]];return;}
 self.handle=h;
}
- (void)cullReplay {
 _defer=NO;
 std::vector<CullEntry> ops;ops.swap(_ops);
 // sweet cull: a draw E is dead if a later covering draw D overwrites E's whole clip on every target E writes,
 // and no surviving draw between them touches E's clip (it could read what E left, e.g. programmable blending)
 std::vector<char> dead(ops.size(),0);long last=-1;
 auto inside=[](const CullEntry &a,const CullEntry &b){return a.cx0>=b.cx0&&a.cy0>=b.cy0&&a.cx1<=b.cx1&&a.cy1<=b.cy1;};
 auto overlap=[](const CullEntry &a,const CullEntry &b){return a.cx0<b.cx1&&b.cx0<a.cx1&&a.cy0<b.cy1&&b.cy0<a.cy1;};
 if(!_noCull&&_cullPass)for(long d=(long)ops.size()-1;d>=0;d--) {
  if(!ops[d].draw||!ops[d].covering||dead[d])continue;
  for(long e=d-1;e>=0;e--) {
   if(!ops[e].draw||dead[e]||!inside(ops[e],ops[d])||(ops[e].writes&~ops[d].over))continue;
   bool safe=true;
   for(long m=e+1;m<d&&safe;m++)if(ops[m].draw&&!dead[m]&&overlap(ops[m],ops[e]))safe=false;
   if(safe){dead[e]=1;last=std::max(last,d);}
  }
 }
 {static std::atomic<int> dumps{0};size_t nd=0;bool cov=false;for(auto &o:ops)if(o.draw){nd++;cov|=o.covering;}   // sweet cull: dump the first big passes
  if(nd>=10&&cov&&dumps++<3)for(size_t i=0;i<ops.size();i++)if(ops[i].draw){const CullEntry &o=ops[i];NSString *lb=o.label?[o.label stringByReplacingOccurrencesOfString:@"com.apple.coreanimation.draw." withString:@""]:@"-";
   char t[240];snprintf(t,sizeof t,"SWEET CULL: d%zu %s clip %ld,%ld,%ld,%ld w%x o%x cov%d dead%d",i,lb.UTF8String,o.cx0,o.cy0,o.cx1,o.cy1,o.writes,o.over,(int)o.covering,(int)dead[i]);sm2_logs(t);}}
 if(_dfOn)[self dfResolve:ops dead:dead];   // sweet deadfill
 unsigned culled=0;
 for(size_t i=0;i<ops.size();i++) {
  if(dead[i]){culled++;continue;}
  ops[i].blk();
 }
 gCullDraws+=culled;
 const unsigned n=++gCullPasses;
 if(n==1||(culled&&gCullDraws.load()==culled)){char t[96];snprintf(t,sizeof t,"SWEET CULL: replay ops %zu last %ld culled %u",ops.size(),last,culled);sm2_logs(t);}   // sweet cull: first replay, first cull
 if(n%300==0){char t[96];snprintf(t,sizeof t,"SWEET CULL: passes %u culled draws %u",n,gCullDraws.load());sm2_logs(t);}
}
- (void)buffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset index:(NSUInteger)index stage:(uint32_t)stage {
 if(!_defer){[super buffer:buffer offset:offset index:index stage:stage];return;}
 if(stage==1&&index==1){_cvb=[(id)buffer isKindOfClass:[NBBuffer class]]?(NBBuffer *)buffer:nil;_cvbOff=offset;}   // sweet cull
 if(stage==1&&index<8){_dfVb[index]=[(id)buffer isKindOfClass:[NBBuffer class]]?(NBBuffer *)buffer:nil;_dfVbOff[index]=offset;_dfVbytes[index].clear();}   // sweet deadfill
 [self rec:^{[super buffer:buffer offset:offset index:index stage:stage];}];
}
- (void)offset:(NSUInteger)offset index:(NSUInteger)index stage:(uint32_t)stage {
 if(!_defer){[super offset:offset index:index stage:stage];return;}
 if(stage==1&&index==1)_cvbOff=offset;
 if(stage==1&&index<8)_dfVbOff[index]=offset;   // sweet deadfill
 [self rec:^{[super offset:offset index:index stage:stage];}];
}
- (void)texture:(id<MTLTexture>)texture index:(NSUInteger)index stage:(uint32_t)stage {
 if(!_defer){[super texture:texture index:index stage:stage];return;}
 [self rec:^{[super texture:texture index:index stage:stage];}];
}
- (void)sampler:(id<MTLSamplerState>)sampler index:(NSUInteger)index stage:(uint32_t)stage {
 if(!_defer){[super sampler:sampler index:index stage:stage];return;}
 [self rec:^{[super sampler:sampler index:index stage:stage];}];
}
- (void)bytes:(const void *)bytes length:(NSUInteger)length index:(NSUInteger)index stage:(uint32_t)stage {
 if(!_defer){[super bytes:bytes length:length index:index stage:stage];return;}
 if(stage==1&&index==1)_cvb=nil;
 if(stage==1&&index<8){_dfVb[index]=nil;_dfVbytes[index].assign((const uint8_t *)bytes,(const uint8_t *)bytes+(bytes?std::min<NSUInteger>(length,256):0));}   // sweet deadfill
 NSData *copy=[NSData dataWithBytes:bytes length:length];   // transient: setVertexBytes/setFragmentBytes contents
 [self rec:^{[super bytes:copy.bytes length:length index:index stage:stage];}];
}
- (void)fence:(id<MTLFence>)fence stages:(uint64_t)stages update:(bool)update {
 if(!_defer){[super fence:fence stages:stages update:update];return;}
 _noCull=YES;   // sweet cull: fences order work against draws, keep the pass unchanged
 [self rec:^{[super fence:fence stages:stages update:update];}];
}
- (void)drawLogStart:(MTLRenderPassDescriptor *)d {
 if(deadfillMode()) {   // sweet deadfill: the composited frame's passes are deferred (and tracked) too
  id<MTLTexture> t=d.colorAttachments[0].texture;const MTLPixelFormat pf=t?t.pixelFormat:MTLPixelFormatInvalid;
  if(t&&(pf==MTLPixelFormatBGRA8Unorm||pf==MTLPixelFormatRGBA8Unorm)&&t.sampleCount<=1&&t.textureType==MTLTextureType2D&&
     [(id)t isKindOfClass:[NBTexture class]]&&((NBTexture *)t).surface&&!d.depthAttachment.texture&&!d.stencilAttachment.texture&&
     !d.visibilityResultBuffer&&d.colorAttachments[0].level==0&&d.colorAttachments[0].slice==0) {
   _dfOn=YES;_defer=YES;_tw=t.width;_th=t.height;_dfAttached=0;
   for(unsigned i=1;i<8;i++)if(id<MTLTexture> c=d.colorAttachments[i].texture)if(c.storageMode!=MTLStorageModeMemoryless)_dfAttached|=1u<<i;
  }
 }
 if(cullEnabled()) {   // sweet cull: defer only single-colour-target passes without depth/stencil or visibility results
  id<MTLTexture> t0=d.colorAttachments[0].texture;bool single=t0!=nil;_attMask=0;   // "single": all colour targets match target 0's size
  for(unsigned i=0;i<8;i++){id<MTLTexture> x=d.colorAttachments[i].texture;if(!x)continue;_attMask|=1u<<i;if(!t0||x.width!=t0.width||x.height!=t0.height)single=false;}
  if(single&&!d.depthAttachment.texture&&!d.stencilAttachment.texture&&!d.visibilityResultBuffer){_defer=YES;_cullPass=YES;_tw=t0.width;_th=t0.height;}
  static std::atomic<int> big{0};if(t0&&t0.width>=1000&&big++<2){char t[400];int o=snprintf(t,sizeof t,"SWEET CULL: big pass");   // sweet cull: one-time attachment dump
   for(unsigned i=0;i<8&&o<350;i++){id<MTLTexture> x=d.colorAttachments[i].texture;if(x)o+=snprintf(t+o,sizeof t-o," c%u=%lux%lu/f%lu/l%lu",i,(unsigned long)x.width,(unsigned long)x.height,(unsigned long)x.pixelFormat,(unsigned long)d.colorAttachments[i].loadAction);}
   if(d.depthAttachment.texture)o+=snprintf(t+o,sizeof t-o," depth f%lu",(unsigned long)d.depthAttachment.texture.pixelFormat);
   if(d.stencilAttachment.texture)o+=snprintf(t+o,sizeof t-o," stencil f%lu",(unsigned long)d.stencilAttachment.texture.pixelFormat);
   if(d.visibilityResultBuffer)o+=snprintf(t+o,sizeof t-o," vis");
   sm2_logs(t);}
  static std::atomic<bool> said{false};if(!said.exchange(true)){char t[160];snprintf(t,sizeof t,"SWEET CULL: first pass %lux%lu single=%d defer=%d",(unsigned long)(t0?t0.width:0),(unsigned long)(t0?t0.height:0),(int)single,(int)_defer);sm2_logs(t);}   // sweet cull: one-time check
 }
 if(!drawLogPath() || gDrawLogBlocks>=20)return;
 id<MTLTexture> t=d.colorAttachments[0].texture;
 if(!t || t.width<1000 || t.height<2000)return;
 _dl.reset(new DrawLog());_dl->w=t.width;_dl->h=t.height;_dl->format=t.pixelFormat;
 _dl->load=d.colorAttachments[0].loadAction;_dl->store=d.colorAttachments[0].storeAction;
 _dl->depth=d.depthAttachment.texture||d.stencilAttachment.texture;
}
- (void)endEncoding {
 if(_defer)[self cullReplay];   // sweet cull
 if(_dl){_dl->finish();_dl.reset();}
 [super endEncoding];
}
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)state {
 if(![(id)state isKindOfClass:[NBRenderPipeline class]]){[self.commandBuffer refuse:@"foreign render pipeline"];return;}
 if(_dl){NBRenderPipeline *p=(NBRenderPipeline *)state;   // sweet drawlog
  _dl->pipeline=(p.label.length?p.label.UTF8String:"-");_dl->pipeline+=" v="+p.vertexName+" f="+p.fragmentName;_dl->blend=p.blend;_dl->mask=p.writeMask;}
 if(_defer){NBRenderPipeline *pp=(NBRenderPipeline *)state;_pipe=pp;[self rec:^{[self check:nmb_render_set_pipeline(self.handle,[pp handle]) what:"setRenderPipelineState"];}];return;}   // sweet cull
 [self check:nmb_render_set_pipeline(self.handle,[(NBRenderPipeline *)state handle]) what:"setRenderPipelineState"];
}
- (void)dlVertexBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o index:(NSUInteger)i {   // sweet drawlog
 if(_dl && i<31 && (!b || [(id)b isKindOfClass:[NBBuffer class]])){_dl->vbs[i].b=(NBBuffer *)b;_dl->vbs[i].offset=o;}
}
- (void)setVertexBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i {[self dlVertexBuffer:b offset:o index:i];[self buffer:b offset:o index:i stage:1];}
- (void)setFragmentBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i {[self buffer:b offset:o index:i stage:2];}
- (void)setVertexBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i {if(_dl && i<31)_dl->vbs[i].offset=o;[self offset:o index:i stage:1];}
- (void)setFragmentBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i {[self offset:o index:i stage:2];}
- (void)setVertexBuffers:(const id<MTLBuffer> __unsafe_unretained [])b offsets:(const NSUInteger [])o withRange:(NSRange)r {
 for(NSUInteger k=0;k<r.length;k++){[self dlVertexBuffer:b[k] offset:o[k] index:r.location+k];[self buffer:b[k] offset:o[k] index:r.location+k stage:1];}
}
- (void)setFragmentBuffers:(const id<MTLBuffer> __unsafe_unretained [])b offsets:(const NSUInteger [])o withRange:(NSRange)r {
 for(NSUInteger k=0;k<r.length;k++)[self buffer:b[k] offset:o[k] index:r.location+k stage:2];
}
- (void)setVertexBytes:(const void *)p length:(NSUInteger)n atIndex:(NSUInteger)i {if(_dl)_dl->bytes(_dl->vbytes,p,n,i,16);[self bytes:p length:n index:i stage:1];}
- (void)setFragmentBytes:(const void *)p length:(NSUInteger)n atIndex:(NSUInteger)i {if(_dl)_dl->bytes(_dl->fbytes,p,n,i,8);[self bytes:p length:n index:i stage:2];}
- (void)setVertexTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i {[self texture:t index:i stage:1];}
- (void)dlFragmentTexture:(id<MTLTexture>)t index:(NSUInteger)i {   // sweet drawlog
 if(_dl && i<16 && (!t || [(id)t isKindOfClass:[NBTexture class]]))_dl->fts[i].t=(NBTexture *)t;
}
- (void)setFragmentTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i {[self dlFragmentTexture:t index:i];[self texture:t index:i stage:2];}
- (void)setFragmentTextures:(const id<MTLTexture> __unsafe_unretained [])t withRange:(NSRange)r {
 for(NSUInteger k=0;k<r.length;k++){[self dlFragmentTexture:t[k] index:r.location+k];[self texture:t[k] index:r.location+k stage:2];}
}
- (void)setVertexSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i {[self sampler:s index:i stage:1];}
- (void)setFragmentSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i {if(_dl && i<16)_dl->fts[i].sampler=s!=nil;[self sampler:s index:i stage:2];}
- (void)setDepthStencilState:(id<MTLDepthStencilState>)state {
 if(state && ![(id)state isKindOfClass:[NBDepthStencil class]]){[self.commandBuffer refuse:@"foreign depth/stencil state"];return;}
 if(state)_dfDss=YES;   // sweet deadfill
 if(_defer){[self rec:^{[self check:nmb_render_set_depth_stencil(self.handle,state?[(NBDepthStencil *)state handle]:nullptr) what:"setDepthStencilState"];}];return;}   // sweet cull
 [self check:nmb_render_set_depth_stencil(self.handle,state?[(NBDepthStencil *)state handle]:nullptr) what:"setDepthStencilState"];
}
- (void)setViewport:(MTLViewport)v {
 const double values[6]={v.originX,v.originY,v.width,v.height,v.znear,v.zfar};
 if(_dl){_dl->viewport=true;memcpy(_dl->vp,values,sizeof values);}   // sweet drawlog
 if(_defer){_vpSet=YES;memcpy(_cvp,values,sizeof values);   // sweet cull
  [self rec:^{const double vv[6]={v.originX,v.originY,v.width,v.height,v.znear,v.zfar};[self check:nmb_render_set_viewport(self.handle,vv) what:"setViewport"];}];return;}
 [self check:nmb_render_set_viewport(self.handle,values) what:"setViewport"];
}
- (void)setScissorRect:(MTLScissorRect)r {
 const uint64_t values[4]={r.x,r.y,r.width,r.height};
 if(_dl){_dl->scissor=true;for(int k=0;k<4;k++)_dl->sc[k]=values[k];}   // sweet drawlog
 if(_defer){_scSet=YES;for(int k=0;k<4;k++)_csc[k]=values[k];   // sweet cull
  [self rec:^{const uint64_t sv[4]={r.x,r.y,r.width,r.height};[self check:nmb_render_set_scissor(self.handle,sv) what:"setScissorRect"];}];return;}
 [self check:nmb_render_set_scissor(self.handle,values) what:"setScissorRect"];
}
// The runtime takes cull mode, winding and fill mode together; this encoder keeps Metal's defaults until each is set.
- (void)raster {
 NSNumber *cull=objc_getAssociatedObject(self,"cull"),*winding=objc_getAssociatedObject(self,"winding"),*fill=objc_getAssociatedObject(self,"fill");
 const unsigned long long cv=cull.unsignedLongLongValue,wv=winding.unsignedLongLongValue,fv=fill.unsignedLongLongValue;
 if(_defer){[self rec:^{[self check:nmb_render_set_raster(self.handle,cv,wv,fv) what:"raster state"];}];return;}   // sweet cull
 [self check:nmb_render_set_raster(self.handle,cv,wv,fv) what:"raster state"];
}
- (void)setCullMode:(MTLCullMode)m {objc_setAssociatedObject(self,"cull",@(m),OBJC_ASSOCIATION_RETAIN);[self raster];}
- (void)setFrontFacingWinding:(MTLWinding)w {objc_setAssociatedObject(self,"winding",@(w),OBJC_ASSOCIATION_RETAIN);[self raster];}
- (void)setTriangleFillMode:(MTLTriangleFillMode)f {objc_setAssociatedObject(self,"fill",@(f),OBJC_ASSOCIATION_RETAIN);[self raster];}
- (void)setDepthBias:(float)bias slopeScale:(float)slope clamp:(float)clamp {
 if(_defer){[self rec:^{[self check:nmb_render_set_depth_bias(self.handle,bias,slope,clamp) what:"setDepthBias"];}];return;}   // sweet cull
 [self check:nmb_render_set_depth_bias(self.handle,bias,slope,clamp) what:"setDepthBias"];
}
// 0109 (B24): Maps aborts on the missing selector. Clip (0) is Vulkan's default; the runtime has no depth clamp, so
// Clamp is stored and logged once. Log off: M2V_B24_LOG=0.
- (void)setDepthClipMode:(MTLDepthClipMode)mode {
 objc_setAssociatedObject(self,"depthClip",@(mode),OBJC_ASSOCIATION_RETAIN);
 static BOOL warned=NO;
 const char *off=getenv("M2V_B24_LOG");
 if(mode==MTLDepthClipModeClamp&&!warned&&!(off&&!strcmp(off,"0"))){warned=YES;NBLOG(@"native-metal: setDepthClipMode: Clamp not supported, ignored");}
}
- (void)setLineWidth:(float)width {
 if(_defer){[self rec:^{[self check:nmb_render_set_line_width(self.handle,width) what:"setLineWidth"];}];return;}   // sweet cull
 [self check:nmb_render_set_line_width(self.handle,width) what:"setLineWidth"];}
- (void)setVisibilityResultMode:(MTLVisibilityResultMode)mode offset:(NSUInteger)offset {
 if(_defer){_cvis=mode;[self rec:^{[self check:nmb_render_set_visibility_mode(self.handle,mode,offset) what:"setVisibilityResultMode"];}];return;}   // sweet cull
 [self check:nmb_render_set_visibility_mode(self.handle,mode,offset) what:"setVisibilityResultMode"];
}
- (void)setFragmentTexture:(id<MTLTexture>)t atTextureIndex:(NSUInteger)ti samplerState:(id<MTLSamplerState>)s atSamplerIndex:(NSUInteger)si {
 [self texture:t index:ti stage:2];[self sampler:s index:si stage:2];
}
// Tile shaders and imageblock tile memory (Apple GPU families 4 and up) are not offered: supportsTileShaders answers no, and a call that
// reaches one anyway fails its command buffer.
- (NSUInteger)tileWidth {return 0;}
- (NSUInteger)tileHeight {return 0;}
- (void)setTileTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i {(void)t;(void)i;[self.commandBuffer refuse:@"tile shaders are not supported"];}
- (void)setTileBytes:(const void *)p length:(NSUInteger)n atIndex:(NSUInteger)i {(void)p;(void)n;(void)i;[self.commandBuffer refuse:@"tile shaders are not supported"];}
- (void)dispatchThreadsPerTile:(MTLSize)size {(void)size;[self.commandBuffer refuse:@"tile shaders are not supported"];}
- (void)dispatchThreadsPerTile:(MTLSize)size inRegion:(MTLRegion)region {(void)size;(void)region;[self.commandBuffer refuse:@"tile shaders are not supported"];}
- (void)setStencilReferenceValue:(uint32_t)v {
 if(_defer){[self rec:^{[self check:nmb_render_set_stencil_reference(self.handle,v,v) what:"setStencilReferenceValue"];}];return;}   // sweet cull
 [self check:nmb_render_set_stencil_reference(self.handle,v,v) what:"setStencilReferenceValue"];}
- (void)setStencilFrontReferenceValue:(uint32_t)f backReferenceValue:(uint32_t)b {
 if(_defer){[self rec:^{[self check:nmb_render_set_stencil_reference(self.handle,f,b) what:"setStencilReferenceValues"];}];return;}   // sweet cull
 [self check:nmb_render_set_stencil_reference(self.handle,f,b) what:"setStencilReferenceValues"];
}
- (void)setBlendColorRed:(float)r green:(float)g blue:(float)b alpha:(float)a {
 const float c[4]={r,g,b,a};
 if(_defer){[self rec:^{const float c2[4]={r,g,b,a};[self check:nmb_render_set_blend_color(self.handle,c2) what:"setBlendColor"];}];return;}   // sweet cull
 [self check:nmb_render_set_blend_color(self.handle,c) what:"setBlendColor"];
}
- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)s vertexCount:(NSUInteger)n instanceCount:(NSUInteger)i baseInstance:(NSUInteger)b {
 gDraws++;self.commandBuffer->encodedWork.draws++;
 if(_dl)_dl->draw(t,s,n,i,false);   // sweet drawlog
 if(_defer){   // sweet cull
  const bool cv=[self coversPrim:t start:s count:n inst:i indexed:false indexType:MTLIndexTypeUInt16 indexBuffer:nil indexOffset:0 baseVertex:0];
  [self recDraw:^{[self check:nmb_render_draw(self.handle,t,(uint32_t)s,(uint32_t)n,(uint32_t)i,(uint32_t)b) what:"drawPrimitives"];} covering:cv];
  if(_dfOn)[self dfNote:t start:s count:n inst:i indexBuffer:nil type:MTLIndexTypeUInt16 offset:0 base:0];   // sweet deadfill
  return;
 }
 [self check:nmb_render_draw(self.handle,t,(uint32_t)s,(uint32_t)n,(uint32_t)i,(uint32_t)b) what:"drawPrimitives"];
}
- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)s vertexCount:(NSUInteger)n instanceCount:(NSUInteger)i {
 [self drawPrimitives:t vertexStart:s vertexCount:n instanceCount:i baseInstance:0];
}
- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)s vertexCount:(NSUInteger)n {
 [self drawPrimitives:t vertexStart:s vertexCount:n instanceCount:1 baseInstance:0];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexCount:(NSUInteger)n indexType:(MTLIndexType)k indexBuffer:(id<MTLBuffer>)b indexBufferOffset:(NSUInteger)o
                instanceCount:(NSUInteger)i baseVertex:(NSInteger)v baseInstance:(NSUInteger)bi {
 if(![(id)b isKindOfClass:[NBBuffer class]]){[self.commandBuffer refuse:@"foreign index buffer"];return;}
 [self.commandBuffer.resourceOwners addObject:b];
 gDraws++;self.commandBuffer->encodedWork.draws++;
 if(_dl)_dl->draw(t,o,n,i,true);   // sweet drawlog (idx=offset/count)
 if(_defer){   // sweet cull
  const bool cv=[self coversPrim:t start:0 count:n inst:i indexed:true indexType:k indexBuffer:(NBBuffer *)b indexOffset:o baseVertex:v];
  [self recDraw:^{[self check:nmb_render_draw_indexed(self.handle,t,(uint32_t)n,k,[(NBBuffer *)b handle],o,(uint32_t)i,(int32_t)v,(uint32_t)bi) what:"drawIndexedPrimitives"];} covering:cv];
  if(_dfOn)[self dfNote:t start:0 count:n inst:i indexBuffer:(NBBuffer *)b type:k offset:o base:v];   // sweet deadfill
  return;
 }
 [self check:nmb_render_draw_indexed(self.handle,t,(uint32_t)n,k,[(NBBuffer *)b handle],o,(uint32_t)i,(int32_t)v,(uint32_t)bi) what:"drawIndexedPrimitives"];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexCount:(NSUInteger)n indexType:(MTLIndexType)k indexBuffer:(id<MTLBuffer>)b indexBufferOffset:(NSUInteger)o
                instanceCount:(NSUInteger)i {
 [self drawIndexedPrimitives:t indexCount:n indexType:k indexBuffer:b indexBufferOffset:o instanceCount:i baseVertex:0 baseInstance:0];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexCount:(NSUInteger)n indexType:(MTLIndexType)k indexBuffer:(id<MTLBuffer>)b indexBufferOffset:(NSUInteger)o {
 [self drawIndexedPrimitives:t indexCount:n indexType:k indexBuffer:b indexBufferOffset:o instanceCount:1 baseVertex:0 baseInstance:0];
}
- (void)useResource:(id<MTLResource>)r usage:(MTLResourceUsage)u stages:(MTLRenderStages)s {(void)r;(void)u;(void)s;}
// 0110 (B24): more selectors Maps-like apps call. Residency and barriers are no-ops like useResource; the multi
// scissor/viewport forms keep element 0 only (logged once if more are given).
- (void)useResources:(const id<MTLResource> __unsafe_unretained*)r count:(NSUInteger)n usage:(MTLResourceUsage)u stages:(MTLRenderStages)s {(void)r;(void)n;(void)u;(void)s;}
- (void)useHeap:(id<MTLHeap>)h {(void)h;}
- (void)useHeap:(id<MTLHeap>)h stages:(MTLRenderStages)s {(void)h;(void)s;}
- (void)useHeaps:(const id<MTLHeap> __unsafe_unretained*)h count:(NSUInteger)n stages:(MTLRenderStages)s {(void)h;(void)n;(void)s;}
- (void)memoryBarrierWithScope:(MTLBarrierScope)sc afterStages:(MTLRenderStages)a beforeStages:(MTLRenderStages)b {(void)sc;(void)a;(void)b;}
- (void)setScissorRects:(const MTLScissorRect *)r count:(NSUInteger)n {
 static BOOL warned=NO;
 if(n>1&&!warned){warned=YES;NBLOG(@"native-metal: setScissorRects: count %lu, only the first used",(unsigned long)n);}
 if(n)[self setScissorRect:r[0]];
}
- (void)setViewports:(const MTLViewport *)v count:(NSUInteger)n {
 static BOOL warned=NO;
 if(n>1&&!warned){warned=YES;NBLOG(@"native-metal: setViewports: count %lu, only the first used",(unsigned long)n);}
 if(n)[self setViewport:v[0]];
}
- (void)updateFence:(id<MTLFence>)fence afterStages:(MTLRenderStages)stages {[self fence:fence stages:stages update:true];}
- (void)waitForFence:(id<MTLFence>)fence beforeStages:(MTLRenderStages)stages {[self fence:fence stages:stages update:false];}
@end

@interface NBBlitEncoder : NBEncoder
@end
@implementation NBBlitEncoder
// The arguments are passed as a plain array: an NSArray literal raises on a nil element, and a nil resource (a refused allocation) must fail the
// command buffer like any invalid argument, not abort the host process.
#define NB_OBJECTS(...) ((__unsafe_unretained id[]){__VA_ARGS__})
- (bool)buffers:(__unsafe_unretained id *)objects count:(size_t)count {
 for(size_t i=0;i<count;i++)if(![objects[i] isKindOfClass:[NBBuffer class]]){[self.commandBuffer refuse:@"missing or foreign blit buffer"];return false;}
 for(size_t i=0;i<count;i++)[self.commandBuffer.resourceOwners addObject:objects[i]];
 return true;
}
- (bool)textures:(__unsafe_unretained id *)objects count:(size_t)count {
 for(size_t i=0;i<count;i++) {
  id o=objects[i];
  if(![o isKindOfClass:[NBTexture class]]){[self.commandBuffer refuse:@"missing or foreign blit texture"];return false;}
  [self.commandBuffer.resourceOwners addObject:o];
  if([(NBTexture *)o surface])[self.commandBuffer.surfaces addObject:o];
 }
 return true;
}
- (void)copyFromBuffer:(id<MTLBuffer>)s sourceOffset:(NSUInteger)so toBuffer:(id<MTLBuffer>)d destinationOffset:(NSUInteger)dO size:(NSUInteger)n {
 if(![self buffers:NB_OBJECTS(s,d) count:2])return;
 [self check:nmb_blit_copy_buffer(self.handle,[(NBBuffer *)s handle],so,[(NBBuffer *)d handle],dO,n) what:"copyFromBuffer:toBuffer:"];
}
- (void)fillBuffer:(id<MTLBuffer>)b range:(NSRange)r value:(uint8_t)v {
 if(![self buffers:NB_OBJECTS(b) count:1])return;
 [self check:nmb_blit_fill_buffer(self.handle,[(NBBuffer *)b handle],r.location,r.location+r.length,v) what:"fillBuffer"];
}
- (void)copyFromTexture:(id<MTLTexture>)s sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl sourceOrigin:(MTLOrigin)so sourceSize:(MTLSize)z
              toTexture:(id<MTLTexture>)d destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl destinationOrigin:(MTLOrigin)dO {
 if(![self textures:NB_OBJECTS(s,d) count:2])return;
 const uint64_t a[3]={so.x,so.y,so.z},size[3]={z.width,z.height,z.depth},b[3]={dO.x,dO.y,dO.z};
 [self check:nmb_blit_copy_texture(self.handle,[(NBTexture *)s handle],ss,sl,a,size,[(NBTexture *)d handle],ds,dl,b) what:"copyFromTexture:toTexture:"];
}
- (void)copyFromTexture:(id<MTLTexture>)s toTexture:(id<MTLTexture>)d {
 if(![self textures:NB_OBJECTS(s,d) count:2])return;
 const NSUInteger levels=MIN(s.mipmapLevelCount,d.mipmapLevelCount),slices=MIN(s.arrayLength,d.arrayLength);
 for(NSUInteger slice=0;slice<slices;slice++)for(NSUInteger level=0;level<levels;level++) {
  MTLSize size=MTLSizeMake(MAX(s.width>>level,(NSUInteger)1),MAX(s.height>>level,(NSUInteger)1),MAX(s.depth>>level,(NSUInteger)1));
  [self copyFromTexture:s sourceSlice:slice sourceLevel:level sourceOrigin:MTLOriginMake(0,0,0) sourceSize:size toTexture:d destinationSlice:slice
       destinationLevel:level destinationOrigin:MTLOriginMake(0,0,0)];
 }
}
- (void)copyFromBuffer:(id<MTLBuffer>)s sourceOffset:(NSUInteger)o sourceBytesPerRow:(NSUInteger)row sourceBytesPerImage:(NSUInteger)image
            sourceSize:(MTLSize)z toTexture:(id<MTLTexture>)d destinationSlice:(NSUInteger)slice destinationLevel:(NSUInteger)level
     destinationOrigin:(MTLOrigin)dO {
 if(![self buffers:NB_OBJECTS(s) count:1] || ![self textures:NB_OBJECTS(d) count:1])return;
 const uint64_t size[3]={z.width,z.height,z.depth},b[3]={dO.x,dO.y,dO.z};
 [self check:nmb_blit_buffer_to_texture(self.handle,[(NBBuffer *)s handle],o,row,image,size,[(NBTexture *)d handle],slice,level,b) what:"copyFromBuffer:toTexture:"];
}
- (void)copyFromTexture:(id<MTLTexture>)s sourceSlice:(NSUInteger)slice sourceLevel:(NSUInteger)level sourceOrigin:(MTLOrigin)so sourceSize:(MTLSize)z
               toBuffer:(id<MTLBuffer>)d destinationOffset:(NSUInteger)o destinationBytesPerRow:(NSUInteger)row destinationBytesPerImage:(NSUInteger)image {
 if(![self textures:NB_OBJECTS(s) count:1] || ![self buffers:NB_OBJECTS(d) count:1])return;
 const uint64_t a[3]={so.x,so.y,so.z},size[3]={z.width,z.height,z.depth};
 [self check:nmb_blit_texture_to_buffer(self.handle,[(NBTexture *)s handle],slice,level,a,size,[(NBBuffer *)d handle],o,row,image) what:"copyFromTexture:toBuffer:"];
}
- (void)generateMipmapsForTexture:(id<MTLTexture>)t {
 if(![self textures:NB_OBJECTS(t) count:1])return;
 [self check:nmb_blit_generate_mipmaps(self.handle,[(NBTexture *)t handle]) what:"generateMipmapsForTexture"];
}
- (void)synchronizeResource:(id<MTLResource>)r {(void)r;}
@end

static void completed(void *context);
static void scheduled(void *context);
@implementation NBCommandBuffer
- (instancetype)init {
 if((self=[super init])){_scheduledHandlers=[NSMutableArray array];_completedHandlers=[NSMutableArray array];_surfaces=[NSMutableSet set];_resourceOwners=[NSMutableSet set];_handlers=dispatch_group_create();_schedulingHandlers=dispatch_group_create();}
 if(self)createdUs=nowUs();
 return self;
}
- (void)dealloc {nmb_command_buffer_release(_handle);}
- (id<MTLCommandQueue>)commandQueue {return (id<MTLCommandQueue>)_queue;}
- (BOOL)retainedReferences {return YES;}
// Protected content is not supported (the runtime's AppleOnlyCall::SetProtectionOptions takes Metal's default only): any other option fails
// the command buffer.
- (uint64_t)protectionOptions {return 0;}
- (void)setProtectionOptions:(uint64_t)options {if(options)[self refuse:[NSString stringWithFormat:@"protection options 0x%llx",options]];}
- (void)refuse:(NSString *)what {
 if(!self.encodeFailure){self.encodeFailure=what;NBLOG(@"native-metal: command buffer refused: %@",what);}
}
- (MTLCommandBufferStatus)status {
 if(self.failedLocally)return MTLCommandBufferStatusError;
 return (MTLCommandBufferStatus)nmb_command_buffer_status(_handle);
}
- (NSError *)error {
 if(self.failedLocally)return nbError(self.encodeFailure);
 if(self.status!=MTLCommandBufferStatusError)return nil;
 char text[512]={};nmb_command_buffer_error(_handle,text,sizeof text);
 return nbError([NSString stringWithUTF8String:text]);
}
// Debug groups and cache hints name work for tools and the driver's caches; they change nothing the GPU computes here.
- (void)pushDebugGroup:(NSString *)name {(void)name;}
- (void)popDebugGroup {}
- (void)encodeCacheHintTag:(uint64_t)tag resourceGroups:(const id __unsafe_unretained [])groups count:(NSUInteger)count {(void)tag;(void)groups;(void)count;}
- (void)encodeCacheHintFinalize:(uint64_t)tag resourceGroups:(const id __unsafe_unretained [])groups count:(NSUInteger)count {(void)tag;(void)groups;(void)count;}
// The kernel driver's own submission times are not recorded: 0 is Metal's answer where they are unavailable.
- (CFTimeInterval)kernelStartTime {return 0;}
- (CFTimeInterval)kernelEndTime {return 0;}
- (BOOL)commitAndWaitUntilSubmitted {[(id<MTLCommandBuffer>)self commit];[self waitUntilScheduled];return !self.failedLocally;}
- (CFTimeInterval)GPUStartTime {return nmb_command_buffer_gpu_time(_handle,false);}
- (CFTimeInterval)GPUEndTime {return nmb_command_buffer_gpu_time(_handle,true);}
- (id)encoder:(NBEncoder *)encoder handle:(NmbEncoder *)handle what:(NSString *)what {
 if(self.committed){[self refuse:[what stringByAppendingString:@" after commit"]];return nil;}
 if(!handle){[self refuse:[NSString stringWithFormat:@"%@: %@",what,lastError()]];return nil;}
 encoder.handle=handle;encoder.commandBuffer=self;return encoder;
}
- (id<MTLComputeCommandEncoder>)computeCommandEncoder {
 return [self encoder:[NBComputeEncoder new] handle:self.committed?nullptr:nmb_compute_encoder_new(_handle) what:@"computeCommandEncoder"];
}
- (id<MTLComputeCommandEncoder>)computeCommandEncoderWithDispatchType:(MTLDispatchType)type {
 if(type!=MTLDispatchTypeSerial){[self refuse:@"concurrent compute dispatch"];return nil;}
 return [self computeCommandEncoder];
}
- (id<MTLBlitCommandEncoder>)blitCommandEncoder {
 gBlits++;encodedWork.blits++;
 return [self encoder:[NBBlitEncoder new] handle:self.committed?nullptr:nmb_blit_encoder_new(_handle) what:@"blitCommandEncoder"];
}
- (id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)d {
 NmbRenderPassDescriptor pass={};
 {
  id<MTLTexture> first=d.colorAttachments[0].texture;
  if(first) {
   const uint64_t pixels=(uint64_t)first.width*first.height;
   gPasses++;gPassPixels+=pixels;encodedWork.passes++;encodedWork.pixels+=pixels;
   uint64_t seen=gBiggestPass.load();while(pixels>seen && !gBiggestPass.compare_exchange_weak(seen,pixels)){}
   if(pixels>=2000000)gFullScreenPasses++;
  }
 }
 auto attachment=[&](MTLRenderPassAttachmentDescriptor *a,NmbPassAttachment &out)->bool {
  if(!a.texture)return true;
  if(![(id)a.texture isKindOfClass:[NBTexture class]])return false;
  NBTexture *texture=(NBTexture *)a.texture;if(texture.surface)[_surfaces addObject:texture];
  [_resourceOwners addObject:texture];
  out.texture=texture.handle;out.level=a.level;out.slice=a.slice;out.depth_plane=a.depthPlane;out.load=a.loadAction;out.store=a.storeAction;
  return true;
 };
 for(unsigned i=0;i<8;i++) {
  MTLRenderPassColorAttachmentDescriptor *c=d.colorAttachments[i];
  if(!attachment(c,pass.colors[i])){[self refuse:@"foreign color attachment"];return nil;}
  pass.colors[i].clear[0]=c.clearColor.red;pass.colors[i].clear[1]=c.clearColor.green;pass.colors[i].clear[2]=c.clearColor.blue;pass.colors[i].clear[3]=c.clearColor.alpha;
 }
 if(!attachment(d.depthAttachment,pass.depth) || !attachment(d.stencilAttachment,pass.stencil)){[self refuse:@"foreign depth/stencil attachment"];return nil;}
 pass.depth.clear[0]=d.depthAttachment.clearDepth;pass.stencil.clear[0]=d.stencilAttachment.clearStencil;
 pass.width=d.renderTargetWidth;pass.height=d.renderTargetHeight;pass.array_length=d.renderTargetArrayLength;pass.samples=d.defaultRasterSampleCount;
 if(d.visibilityResultBuffer) {
  if(![(id)d.visibilityResultBuffer isKindOfClass:[NBBuffer class]]){[self refuse:@"foreign visibility result buffer"];return nil;}
  [_resourceOwners addObject:d.visibilityResultBuffer];
  pass.visibility=[(NBBuffer *)d.visibilityResultBuffer handle];
 }
 NBRenderEncoder *encoder=[NBRenderEncoder new];
 [encoder drawLogStart:d];   // sweet drawlog
 if(!self.committed&&[encoder dfHold:pass load:d.colorAttachments[0].loadAction]){encoder.commandBuffer=self;return (id<MTLRenderCommandEncoder>)encoder;}   // sweet deadfill: made at endEncoding
 return [self encoder:encoder handle:self.committed?nullptr:nmb_render_encoder_new(_handle,&pass) what:@"renderCommandEncoder"];
}
- (void)addScheduledHandler:(MTLCommandBufferHandler)block {if(block)@synchronized(self){[_scheduledHandlers addObject:[block copy]];}}
- (void)addCompletedHandler:(MTLCommandBufferHandler)block {if(block)@synchronized(self){[_completedHandlers addObject:[block copy]];}}
- (void)enqueue {}
- (void)event:(id<MTLEvent>)event value:(uint64_t)value signal:(bool)signal {
 if(![(id)event isKindOfClass:[NBEvent class]]){[self refuse:@"foreign event"];return;}
 if(self.committed){[self refuse:@"event encoded after commit"];return;}
 NmbEvent *handle=[(NBEvent *)event handle];
 if(!(signal?nmb_command_buffer_signal_event(_handle,handle,value):nmb_command_buffer_wait_event(_handle,handle,value)))
  [self refuse:[NSString stringWithFormat:@"%s: %@",signal?"encodeSignalEvent":"encodeWaitForEvent",lastError()]];
}
- (void)encodeSignalEvent:(id<MTLEvent>)event value:(uint64_t)value {[self event:event value:value signal:true];}
- (void)encodeWaitForEvent:(id<MTLEvent>)event value:(uint64_t)value {[self event:event value:value signal:false];}
// The runtime has submitted the work. Deliver scheduling without waiting for GPU completion.
- (void)didSchedule {
 {
  const uint64_t took=nowUs()-self.committedUs;
  std::lock_guard<std::mutex> lock(gCommitLock);
  CommitRecord &r=gCommitRing[self.commitIndex%32];
  if(r.index==self.commitIndex)r.scheduledUs=took;
 }
 NSArray *handlers;
 @synchronized(self){
  _schedulingDelivered=YES;handlers=[_scheduledHandlers copy];[_scheduledHandlers removeAllObjects];
 }
 void (^run)(void)=^{
  for(MTLCommandBufferHandler block in handlers)block((id<MTLCommandBuffer>)self);
  dispatch_group_leave(self->_schedulingHandlers);
 };
 // Run on the runtime's scheduled-handler thread, preserving its ordering and own-wait
 // deadlock checks. The queue's completionQueue routes completed handlers only.
 run();
}
// After completion (or a refusal): invalidate the CPU's view of imported surfaces, then completed handlers, once.
- (void)finish {
 if(ctlPath())ctlF0=mach_absolute_time();   // sweet ctl
 flushSurfaces(_surfaces);
 if(ctlPath())ctlF1=mach_absolute_time();
 // Actual GPU completion (or a local refusal that never submitted) is the
 // release point. A retained completed command must not pin every old frame.
 [_surfaces removeAllObjects];[_resourceOwners removeAllObjects];
 @synchronized(gInFlight){[gInFlight removeObject:self];}
 NSArray *completedHandlers;
 @synchronized(self){
  // A frontend refusal or a failed submission never scheduled. Unblock its waiter without inventing a scheduled event.
  if(!_schedulingDelivered){_schedulingDelivered=YES;[_scheduledHandlers removeAllObjects];dispatch_group_leave(_schedulingHandlers);}
  completedHandlers=[_completedHandlers copy];[_completedHandlers removeAllObjects];
 }
 void (^run)(void)=^{
  if(ctlPath())self->ctlQ=mach_absolute_time();   // sweet ctl
  for(MTLCommandBufferHandler block in completedHandlers)block((id<MTLCommandBuffer>)self);
  if(ctlPath()&&self->ctlC){CtlRec r{self.commitIndex,self->ctlC,self->ctlD,self->ctlF0,self->ctlF1,self->ctlQ,mach_absolute_time()};ctlPush(r);}
  dispatch_group_leave(self->_handlers);
 };
 dispatch_queue_t queue=_queue.completionQueue;
 if(queue)dispatch_async(queue,run);else run();
}
- (void)commit {
 if(self.committed){NBLOG(@"native-metal: commit twice");return;}
 self.committed=YES;
 self.committedUs=nowUs();
 if(ctlPath())ctlC=mach_absolute_time();   // sweet ctl
 {
  std::lock_guard<std::mutex> lock(gCommitLock);
  // Assign once under the same lock as publication. A concurrent commit cannot reuse this index.
  const uint32_t index=(uint32_t)(gCommits.fetch_add(1)+1);self.commitIndex=index;
  CommitRecord &r=gCommitRing[index%32];
  r={};r.index=index;r.committedUs=self.committedUs;r.ageUs=self.committedUs-createdUs;r.work=encodedWork;
 }
 dispatch_group_enter(_handlers);
 dispatch_group_enter(_schedulingHandlers);
 @synchronized(gInFlight){[gInFlight addObject:self];}
 if(!self.encodeFailure) {
  flushSurfaces(_surfaces);
  // Completion owns this reference; the runtime orders the non-owning scheduled callback before it.
  void *context=(__bridge_retained void *)self;
  if(nmb_command_buffer_on_completed(_handle,completed,context) &&
     nmb_command_buffer_on_scheduled(_handle,scheduled,context) && nmb_command_buffer_commit(_handle))return;
  (void)CFBridgingRelease(context);   // refused registration/commit cannot call the registered handlers
  [self refuse:[@"commit: " stringByAppendingString:lastError()]];
 }
 self.failedLocally=YES;gCommitFailures++;
 dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT,0),^{[self finish];});
}
- (void)waitUntilCompleted {
 if(!self.committed)return;
 const uint64_t began=nowUs();
 if(!self.failedLocally)nmb_command_buffer_wait(_handle,0);
 dispatch_group_wait(_handlers,DISPATCH_TIME_FOREVER);
 gTimeWaitCompleted.add(began);
}
- (void)waitUntilScheduled {
 if(!self.committed)return;
 const uint64_t began=nowUs();
 if(!self.failedLocally && !nmb_command_buffer_wait_scheduled(_handle)){
  NBLOG(@"native-metal: waitUntilScheduled refused: %@",lastError());return;
 }
 dispatch_group_wait(_schedulingHandlers,DISPATCH_TIME_FOREVER);
 gTimeWaitScheduled.add(began);
}
@end
static void scheduled(void *context) {[(__bridge NBCommandBuffer *)context didSchedule];}
// The runtime's completion: the command buffer's own reference, taken at commit, is given back here.
static void completed(void *context) {
 gCompleted++;
 NBCommandBuffer *commandBuffer=(__bridge_transfer NBCommandBuffer *)context;
 if(ctlPath())commandBuffer->ctlD=mach_absolute_time();   // sweet ctl
 {
  const uint64_t took=nowUs()-commandBuffer.committedUs;
  gTimeEncodeToCommit.count++;gTimeEncodeToCommit.total+=took;
  uint64_t seen=gTimeEncodeToCommit.slowest.load();while(took>seen && !gTimeEncodeToCommit.slowest.compare_exchange_weak(seen,took)){}
  std::lock_guard<std::mutex> lock(gCommitLock);
  CommitRecord &r=gCommitRing[commandBuffer.commitIndex%32];
  if(r.index==commandBuffer.commitIndex)r.callbackUs=took;
 }
 [commandBuffer finish];
}

@implementation NBQueue
- (void)dealloc {nmb_queue_release(_handle);}
- (id<MTLCommandBuffer>)commandBuffer {
 NmbCommandBuffer *handle=nmb_command_buffer_new(_handle);
 if(!handle){NBLOG(@"native-metal: commandBuffer refused: %@",lastError());return nil;}
 NBCommandBuffer *commandBuffer=[NBCommandBuffer new];commandBuffer.handle=handle;commandBuffer.queue=self;return (id<MTLCommandBuffer>)commandBuffer;
}
- (id<MTLCommandBuffer>)commandBufferWithUnretainedReferences {return [self commandBuffer];}
- (void)setSubmissionQueue:(dispatch_queue_t)queue {(void)queue;}   // submission happens on the caller's thread here
// setCompletionQueue: is the property's setter.
// GPU priorities are not applied (one hardware queue): reported as not applied.
- (BOOL)setGPUPriority:(NSUInteger)priority {(void)priority;return NO;}
- (BOOL)setBackgroundGPUPriority:(NSUInteger)priority {(void)priority;return NO;}
- (id<MTLCommandBuffer>)commandBufferWithDescriptor:(MTLCommandBufferDescriptor *)d {(void)d;return [self commandBuffer];}
@end

// ---- the device ----
template <typename T> static T failWith(NSError **error,NSString *what,T value) {
 noteProblem([NSString stringWithFormat:@"%@: %@",what,lastError()]);
 if(error)*error=nbError([NSString stringWithFormat:@"%@: %@",what,lastError()]);
 return value;
}
// Which of QuartzCore's paths asked for a pipeline (int-153: every one was made synchronously on its render thread and no generic pipeline
// was ever asked for): its return addresses as offsets from its image header (header 0x189429000 in the 21H461 cache, so offset +
// 0x189429000 is the address in that image's disassembly), the global feature flags it ORs into every context's flags, and its
// CA_DISABLE_GENERIC_SHADERS, CA_FORCE_GENERIC_SHADERS and CA_DISABLE_PRECOMPILED_PIPELINES debug options (ca_debug_options +0x55..0x57,
// read only where dladdr places that address in QuartzCore).
static std::string quartzCoreCallers() {
 /* SweetMetal2 (iOS 16.7): only the return addresses into QuartzCore, as offsets from its header. UniXNU's version also read
  * MetalContext fields and debug bytes at iPadOS 17.7.11 (21H461) offsets, which mean nothing here. */
 void *frames[24];const int n=backtrace(frames,24);
 std::string out="qc";char item[32];unsigned kept=0;
 for(int i=0;i<n && kept<8;i++) {
  Dl_info info{};
  if(!dladdr(frames[i],&info) || !info.dli_fname || !strstr(info.dli_fname,"/QuartzCore"))continue;
  snprintf(item,sizeof item," %lx",(unsigned long)((uintptr_t)frames[i]-(uintptr_t)info.dli_fbase));out+=item;++kept;
 }
 return out;
}
// What a creation cost beyond its wall time (int-156: a translation the host does in 6 ms took 665 ms on the tablet, whose single core is
// as fast): this thread's CPU time (user and system, microseconds) and the task's page faults, page-ins and context switches. A creation
// whose CPU time is near its wall time computed; one whose faults are many paid the 4 KiB fault path; one far below it waited.
struct NBCost {
 uint64_t user=0,system=0;int64_t faults=0,pageins=0,switches=0;
 static NBCost now() {
  NBCost c;thread_basic_info_data_t t{};mach_msg_type_number_t n=THREAD_BASIC_INFO_COUNT;mach_port_t self=mach_thread_self();
  if(thread_info(self,THREAD_BASIC_INFO,(thread_info_t)&t,&n)==KERN_SUCCESS) {
   c.user=(uint64_t)t.user_time.seconds*1000000+t.user_time.microseconds;c.system=(uint64_t)t.system_time.seconds*1000000+t.system_time.microseconds;
  }
  mach_port_deallocate(mach_task_self(),self);
  task_events_info_data_t e{};n=TASK_EVENTS_INFO_COUNT;
  if(task_info(mach_task_self(),TASK_EVENTS_INFO,(task_info_t)&e,&n)==KERN_SUCCESS){c.faults=e.faults;c.pageins=e.pageins;c.switches=e.csw;}
  return c;
 }
 std::string since(const NBCost &b) const {
  char text[128];snprintf(text,sizeof text," cpu=%llu sys=%llu flt=%lld pin=%lld csw=%lld",(unsigned long long)(user+system-b.user-b.system),
      (unsigned long long)(system-b.system),(long long)(faults-b.faults),(long long)(pageins-b.pageins),(long long)(switches-b.switches));
  return text;
 }
};
// One journal line of a render pipeline creation begun at `began` (microseconds), `refused` its refusal or null. The label is the
// caller's name for the pipeline (QuartzCore's: its spec's encoded name).
static void journalRenderPipeline(MTLRenderPipelineDescriptor *d,uint64_t began,const char *refused,const std::string &cost) {
 if(gJournalPath.empty())return;
 const uint64_t ended=nowUs();uint64_t tid=0;pthread_threadid_np(nullptr,&tid);
 char head[256];
 snprintf(head,sizeof head,"R t=%llu tid=%llu main=%d us=%llu ok=%d samples=%lu topo=%lu ds=%lu/%lu",(unsigned long long)((began-gJournalBegan)/1000),
     (unsigned long long)tid,pthread_main_np(),(unsigned long long)(ended-began),refused?0:1,(unsigned long)d.rasterSampleCount,
     (unsigned long)d.inputPrimitiveTopology,(unsigned long)d.depthAttachmentPixelFormat,(unsigned long)d.stencilAttachmentPixelFormat);
 std::string line=head;
 line+=cost;
 line+=" label=";line+=d.label.length?d.label.UTF8String:"-";
 for(unsigned i=0;i<8;i++) {
  MTLRenderPipelineColorAttachmentDescriptor *a=d.colorAttachments[i];if(a.pixelFormat==MTLPixelFormatInvalid)continue;
  snprintf(head,sizeof head," c%u=%lu:%lu:%d:%lu:%lu:%lu:%lu:%lu:%lu",i,(unsigned long)a.pixelFormat,(unsigned long)a.writeMask,(int)a.blendingEnabled,
      (unsigned long)a.sourceRGBBlendFactor,(unsigned long)a.destinationRGBBlendFactor,(unsigned long)a.rgbBlendOperation,(unsigned long)a.sourceAlphaBlendFactor,
      (unsigned long)a.destinationAlphaBlendFactor,(unsigned long)a.alphaBlendOperation);
  line+=head;
 }
 if(MTLVertexDescriptor *v=d.vertexDescriptor)for(unsigned i=0;i<31;i++) {
  MTLVertexAttributeDescriptor *a=v.attributes[i];if(a.format==MTLVertexFormatInvalid)continue;
  MTLVertexBufferLayoutDescriptor *l=v.layouts[a.bufferIndex<31?a.bufferIndex:0];
  snprintf(head,sizeof head," a%u=%lu:%lu:%lu:%lu:%lu:%lu",i,(unsigned long)a.format,(unsigned long)a.offset,(unsigned long)a.bufferIndex,(unsigned long)l.stride,
      (unsigned long)l.stepFunction,(unsigned long)l.stepRate);
  line+=head;
 }
 NBFunction *vf=[(id)d.vertexFunction isKindOfClass:[NBFunction class]]?(NBFunction *)d.vertexFunction:nil;
 NBFunction *ff=[(id)d.fragmentFunction isKindOfClass:[NBFunction class]]?(NBFunction *)d.fragmentFunction:nil;
 line+=" | v ";line+=vf?vf.name.UTF8String:"-";line+=" ";line+=vf?vf.constantsText:"";
 line+=" | f ";line+=ff?ff.name.UTF8String:"-";line+=" ";line+=ff?ff.constantsText:"";
 if(refused){line+=" | refused ";line+=refused;}
 line+=" | ";line+=quartzCoreCallers();
 for(char &c:line)if(c=='\n')c=' ';
 journal(line);
}
// A render pipeline descriptor in the bridge's form; false, with the error, for a function of another device.
static bool renderDescriptor(MTLRenderPipelineDescriptor *d,NmbRenderPipelineDescriptor *p,NSError **error) {
 if(![(id)d.vertexFunction isKindOfClass:[NBFunction class]] || (d.fragmentFunction && ![(id)d.fragmentFunction isKindOfClass:[NBFunction class]])) {
  if(error)*error=nbError(@"foreign function");return false;
 }
 p->vertex=[(NBFunction *)d.vertexFunction handle];p->fragment=d.fragmentFunction?[(NBFunction *)d.fragmentFunction handle]:nullptr;
 for(unsigned i=0;i<8;i++) {
  MTLRenderPipelineColorAttachmentDescriptor *a=d.colorAttachments[i];
  p->colors[i]={(uint64_t)a.pixelFormat,(uint64_t)a.writeMask,(uint64_t)a.blendingEnabled,(uint64_t)a.sourceRGBBlendFactor,(uint64_t)a.destinationRGBBlendFactor,
      (uint64_t)a.rgbBlendOperation,(uint64_t)a.sourceAlphaBlendFactor,(uint64_t)a.destinationAlphaBlendFactor,(uint64_t)a.alphaBlendOperation};
 }
 p->depth_format=d.depthAttachmentPixelFormat;p->stencil_format=d.stencilAttachmentPixelFormat;p->samples=d.rasterSampleCount;
 p->topology=d.inputPrimitiveTopology;p->rasterization=d.rasterizationEnabled;
 if(MTLVertexDescriptor *v=d.vertexDescriptor) {
  for(unsigned i=0;i<31;i++) {
   MTLVertexAttributeDescriptor *a=v.attributes[i];if(a.format==MTLVertexFormatInvalid)continue;
   p->vertex_inputs=1;p->attributes[i]={(uint64_t)a.format,a.offset,a.bufferIndex};
   if(a.bufferIndex<31){MTLVertexBufferLayoutDescriptor *l=v.layouts[a.bufferIndex];p->layouts[a.bufferIndex]={l.stride,(uint64_t)l.stepFunction,l.stepRate};}
  }
 }
 return true;
}

// ---- heaps ----
// Metal heaps over the runtime's: an automatic heap places each resource, a placement heap takes an offset. Resources made from one keep it
// alive (their `heap`); a heap's resources share its storage mode, CPU cache mode and (here) hazard tracking mode.
@interface NBHeap : NBObject
@property(nonatomic) NmbHeap *handle;
@property(nonatomic) MTLResourceOptions options;
@property(nonatomic) MTLHeapType heapType;
@end
@implementation NBHeap
- (void)dealloc {nmb_heap_release(_handle);}
- (NSUInteger)size {return nmb_heap_size(_handle);}
- (NSUInteger)usedSize {return nmb_heap_used_size(_handle);}
- (NSUInteger)currentAllocatedSize {return nmb_heap_allocated_size(_handle);}
- (NSUInteger)maxAvailableSizeWithAlignment:(NSUInteger)alignment {return nmb_heap_max_available_size(_handle,alignment);}
- (MTLResourceOptions)resourceOptions {return _options;}
- (MTLStorageMode)storageMode {return (MTLStorageMode)((_options&MTLResourceStorageModeMask)>>MTLResourceStorageModeShift);}
- (MTLCPUCacheMode)cpuCacheMode {return (MTLCPUCacheMode)((_options&MTLResourceCPUCacheModeMask)>>MTLResourceCPUCacheModeShift);}
- (MTLHazardTrackingMode)hazardTrackingMode {return (MTLHazardTrackingMode)((_options&MTLResourceHazardTrackingModeMask)>>MTLResourceHazardTrackingModeShift);}
- (MTLHeapType)type {return _heapType;}
- (uint64_t)protectionOptions {return 0;}
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state {
 const uint64_t answer=nmb_heap_set_purgeable_state(_handle,state);
 if(!answer)NBLOG(@"native-metal: heap setPurgeableState refused: %@",lastError());
 return (MTLPurgeableState)answer;
}
- (id<MTLBuffer>)bufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options offset:(uint64_t)offset {
 NmbBuffer *handle=nmb_heap_new_buffer(_handle,length,options,offset);
 if(!handle){NBLOG(@"native-metal: heap buffer refused: %@",lastError());return nil;}
 NBBuffer *buffer=[NBBuffer new];buffer.handle=handle;buffer.options=options;buffer.parentHeap=(id<MTLHeap>)self;
 buffer.heapOffset=offset==UINT64_MAX?0:offset;return (id<MTLBuffer>)buffer;
}
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options {return [self bufferWithLength:length options:options offset:UINT64_MAX];}
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options offset:(NSUInteger)offset {
 return [self bufferWithLength:length options:options offset:offset];
}
- (id<MTLTexture>)textureWithDescriptor:(MTLTextureDescriptor *)d offset:(uint64_t)offset {
 NmbTextureDescriptor descriptor;if(!textureDescriptor(d,&descriptor))return nil;
 NmbTexture *handle=nmb_heap_new_texture(_handle,&descriptor,offset);
 if(!handle){NBLOG(@"native-metal: heap texture refused: %@",lastError());return nil;}
 NBTexture *texture=[NBTexture new];texture.handle=handle;nmb_texture_describe(handle,&descriptor);texture.descriptor=descriptor;
 texture.parentHeap=(id<MTLHeap>)self;texture.heapOffset=offset==UINT64_MAX?0:offset;return (id<MTLTexture>)texture;
}
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d {return [self textureWithDescriptor:d offset:UINT64_MAX];}
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)offset {return [self textureWithDescriptor:d offset:offset];}
@end
// Apple's SPI resource groups name resources for cache hints (encodeCacheHintTag:/Finalize:); a group only keeps its resources alive.
@interface NBResourceGroup : NBObject
@property(nonatomic,strong) NSArray *resources;
@end
@implementation NBResourceGroup
@end

// Creation and archive population must use the same complete supported descriptor.
static bool computeDescriptor(MTLComputePipelineDescriptor *d,NmbComputePipelineDescriptor *p,NSError **error) {
 if(!d || ![(id)d.computeFunction isKindOfClass:[NBFunction class]]){if(error)*error=nbError(@"foreign compute function");return false;}
 // Metal may materialize empty stage/linked descriptors on access. Preserve the
 // default meaning while refusing state this frontend cannot yet represent.
 MTLStageInputOutputDescriptor *stage=d.stageInputDescriptor;
 bool empty=!stage || (stage.indexType==MTLIndexTypeUInt16 && stage.indexBufferIndex==0);
 for(NSUInteger index=0;stage && index<31;++index) {
  MTLAttributeDescriptor *a=stage.attributes[index];MTLBufferLayoutDescriptor *b=stage.layouts[index];
  empty=empty && a.format==MTLAttributeFormatInvalid && !a.offset && !a.bufferIndex && !b.stride &&
        b.stepFunction==MTLStepFunctionPerVertex && b.stepRate==1;
 }
 if(!empty){if(error)*error=nbError(@"compute stage inputs are not implemented");return false;}
 MTLLinkedFunctions *linked=d.linkedFunctions;
 if(linked.functions.count || linked.binaryFunctions.count || linked.privateFunctions.count || linked.groups.count ||
    d.preloadedLibraries.count || d.insertLibraries.count) {
  if(error)*error=nbError(@"compute linked/dynamic libraries are not implemented");return false;
 }
 p->function=[(NBFunction *)d.computeFunction handle];p->max_threads=d.maxTotalThreadsPerThreadgroup;
 p->multiple_of_execution_width=d.threadGroupSizeIsMultipleOfThreadExecutionWidth;p->indirect=d.supportIndirectCommandBuffers;
 p->max_call_stack_depth=d.maxCallStackDepth;p->adding_binary_functions=d.supportAddingBinaryFunctions;
 for(NSUInteger index=0;index<31;++index)p->mutability[index]=d.buffers[index].mutability;
 return true;
}

// ---- binary archives ----
@interface NBBinaryArchive : NBObject
@property(nonatomic) NmbBinaryArchive *handle;
@end
@implementation NBBinaryArchive
- (void)dealloc {nmb_binary_archive_release(_handle);}
- (NSUInteger)options {return 0;}
- (BOOL)addRenderPipelineFunctionsWithDescriptor:(MTLRenderPipelineDescriptor *)d error:(NSError **)error {
 auto *p=new NmbRenderPipelineDescriptor();
 const bool ok=renderDescriptor(d,p,error) && nmb_binary_archive_add_render(_handle,p);delete p;
 if(!ok)return failWith<BOOL>(error,@"archive render pipeline",NO);
 if(error)*error=nil;return YES;
}
- (BOOL)addComputePipelineFunctionsWithDescriptor:(MTLComputePipelineDescriptor *)d error:(NSError **)error {
 NmbComputePipelineDescriptor p={};if(!computeDescriptor(d,&p,error))return NO;
 if(!nmb_binary_archive_add_compute_descriptor(_handle,&p))return failWith<BOOL>(error,@"archive compute pipeline",NO);
 if(error)*error=nil;return YES;
}
- (BOOL)serializeToURL:(NSURL *)url error:(NSError **)error {
 if(!url.isFileURL){if(error)*error=nbError(@"a file URL is required");return NO;}
 if(!nmb_binary_archive_serialize(_handle,url.fileSystemRepresentation))return failWith<BOOL>(error,@"archive serialize",NO);
 if(error)*error=nil;return YES;
}
@end

static bool pipelineArchives(NSArray *list,std::vector<NmbBinaryArchive *> &archives,NSError **error) {
 for(id archive in list) {
  if(![archive isKindOfClass:[NBBinaryArchive class]]){if(error)*error=nbError(@"foreign binary archive");return false;}
  archives.push_back([(NBBinaryArchive *)archive handle]);
 }
 return true;
}

@interface NBDeviceMethods : NSObject
@end
// Bytes of one texel of the uncompressed formats Core Animation and RenderBox use (4 for the rest).
static uint64_t sweetTexelBytes(MTLPixelFormat f) {
 switch((NSUInteger)f) {
 case 1: case 10: case 11: case 12: case 13: case 14: return 1;
 case 20: case 22: case 23: case 24: case 25: case 30: case 32: case 33: case 34: case 40: case 41: case 42: case 43: return 2;
 case 103: case 104: case 105: case 110: case 111: case 112: case 113: case 114: case 115: case 552: case 553: return 8;
 case 123: case 124: case 125: return 16;
 default: return 4;
 }
}
typedef struct {uint64_t w[2];} SweetWatermark;   /* AGXTextureLayout's {?=[2Q]} watermark */
// Block footprint and bytes of the compressed formats the A640 samples (ASTC LDR/sRGB, ETC2/EAC); false for the rest.
static bool sweetBlock(MTLPixelFormat f, uint32_t *bw, uint32_t *bh, uint32_t *bytes) {
 static const uint8_t astc[][2] = {{4,4},{5,4},{5,5},{6,5},{6,6},{0,0},{8,5},{8,6},{8,8},{10,5},{10,6},{10,8},{10,10},{12,10},{12,12}};
 const NSUInteger n = (NSUInteger)f;
 if((n >= 186 && n <= 200) || (n >= 204 && n <= 218)) {
  const NSUInteger i = n >= 204 ? n - 204 : n - 186;
  if(!astc[i][0]) return false;
  *bw = astc[i][0]; *bh = astc[i][1]; *bytes = 16; return true;
 }
 switch(n) {
 case 170: case 172: case 180: case 181: case 182: case 183: *bw = 4; *bh = 4; *bytes = 8; return true;   /* EAC R11, ETC2 RGB8(A1) */
 case 174: case 176: case 178: case 179: *bw = 4; *bh = 4; *bytes = 16; return true;                          /* EAC RG11, RGBA8 */
 default: return false;
 }
}
// Keeps a no-copy texture's memory owner until the texture goes (its deallocator runs then).
@interface SweetMemoryOwner : NSObject
@property(nonatomic,copy) void (^deallocator)(void *,NSUInteger);
@property(nonatomic) void *bytes;
@property(nonatomic) NSUInteger length;
@end
@implementation SweetMemoryOwner
- (void)dealloc {if(_deallocator)_deallocator(_bytes,_length);}
@end
static char sweetMemoryOwnerKey;
// SweetMetal2's MTLTextureLayout (Metal SPI: AGXTextureLayout's protocol on Apple's GPUs): a linear layout of `row`-byte rows.
@interface SweetTextureLayout : NSObject
- (instancetype)initWithRow:(uint64_t)row height:(uint64_t)height texel:(uint64_t)texel;
- (instancetype)initWithBlocks:(uint32_t)bw by:(uint32_t)bh bytes:(uint32_t)bytes width:(uint64_t)width height:(uint64_t)height;
@end
@implementation SweetTextureLayout {
 uint64_t _row,_height,_texel;
 uint32_t _bw,_bh;     /* block footprint (1x1 uncompressed); _height and _texel count block rows and block bytes */
}
- (instancetype)initWithRow:(uint64_t)row height:(uint64_t)height texel:(uint64_t)texel {
 if((self=[super init])){_row=row;_height=height;_texel=texel?texel:4;_bw=_bh=1;}
 return self;
}
- (instancetype)initWithBlocks:(uint32_t)bw by:(uint32_t)bh bytes:(uint32_t)bytes width:(uint64_t)width height:(uint64_t)height {
 if((self=[super init])){_bw=bw;_bh=bh;_texel=bytes;_row=(width+bw-1)/bw*bytes;_height=(height+bh-1)/bh;}
 return self;
}
- (NSUInteger)size {const uint64_t page=(uint64_t)getpagesize();return (NSUInteger)((_row*_height+page-1)&~(page-1));}
- (NSUInteger)alignment {return (NSUInteger)getpagesize();}
- (MTLSize)copyGranularity {return MTLSizeMake(_bw,_bh,1);}
- (SweetWatermark)watermark {SweetWatermark none={{0,0}};return none;}
- (void)initializeTextureMemory:(void *)memory {(void)memory;}
- (void)finalizeTextureMemory:(void *)memory {(void)memory;}
- (void)copyFromLinearBytes:(const void *)src linearOffset:(NSUInteger)offset linearBytesPerRow:(NSUInteger)srcRow linearBytesPerImage:(NSUInteger)image
            toTextureMemory:(void *)dst textureSlice:(NSUInteger)slice textureLevel:(NSUInteger)level textureRegion:(MTLRegion)r {
 (void)image;if(slice || level)return;
 const NSUInteger x0=r.origin.x/_bw,y0=r.origin.y/_bh,w=(r.size.width+_bw-1)/_bw,h=(r.size.height+_bh-1)/_bh;
 for(NSUInteger y=0;y<h && y0+y<_height;y++)
  memcpy((uint8_t *)dst+(y0+y)*_row+x0*_texel,(const uint8_t *)src+offset+y*srcRow,w*_texel);
}
- (void)copyFromTextureMemory:(const void *)src textureSlice:(NSUInteger)slice textureLevel:(NSUInteger)level textureRegion:(MTLRegion)r
              toLinearBytes:(void *)dst linearOffset:(NSUInteger)offset linearBytesPerRow:(NSUInteger)dstRow linearBytesPerImage:(NSUInteger)image {
 (void)image;if(slice || level)return;
 const NSUInteger x0=r.origin.x/_bw,y0=r.origin.y/_bh,w=(r.size.width+_bw-1)/_bw,h=(r.size.height+_bh-1)/_bh;
 for(NSUInteger y=0;y<h && y0+y<_height;y++)
  memcpy((uint8_t *)dst+offset+y*dstRow,(const uint8_t *)src+(y0+y)*_row+x0*_texel,w*_texel);
}
@end
@implementation NBDeviceMethods
- (NSString *)name {return gName;}
- (uint64_t)registryID {return gRegistryID;}
- (NSUInteger)maxBufferLength {return nmb_device_max_buffer_length(gDevice);}
- (BOOL)hasUnifiedMemory {return YES;}
- (BOOL)isLowPower {return NO;}
- (BOOL)isRemovable {return NO;}
- (BOOL)isHeadless {return NO;}
// supportsFamily: and the other queries the runtime answers are installed from it (installAnswers); supportsFamily: also answers the target
// hardware's families (the bridge's TARGET_FAMILIES, journaled with their waived rows as F lines). No legacy feature set is claimed: the
// base implementation reads Apple tables this device does not have.
- (BOOL)supportsFeatureSet:(MTLFeatureSet)set {(void)set;return NO;}
// The original discovery (_MTLAddDevice) runs these four on every device it registers. The target's own versions fill the base class's limits
// and feature tables from a hardware profile (`featureProfile`, `indirectArgumentBufferCapabilities`, a table of Apple GPUs) that this device
// has not got: running them would claim an Apple GPU's capabilities. They are empty here, so a limit or feature the runtime measured is answered
// by this file's own answers (installAnswers) and any other stays the base class's neutral 0/NO. Nothing is claimed that nothing measured.
- (void)initLimits {}
- (void)initFeatureQueries {}
- (void)initGPUFamilySupport {}
- (void)initWorkarounds {}
- (NSString *)productName {return gName;}
- (NSString *)vendorName {char name[64]={};nmb_device_vendor_name(gDevice,name,sizeof name);return [NSString stringWithUTF8String:name];}
- (MTLSize)maxThreadsPerThreadgroup {
 uint64_t s[3]={};if(!nmb_device_max_threads_per_threadgroup(gDevice,s))NBLOG(@"native-metal: maxThreadsPerThreadgroup: %@",lastError());
 return MTLSizeMake(s[0],s[1],s[2]);
}
- (void)doesNotRecognizeSelector:(SEL)selector {
 if(gMissing)gMissing("NativeBridgeMetalDevice",sel_getName(selector));
 noteProblem([NSString stringWithFormat:@"missing device %s",sel_getName(selector)]);
 [super doesNotRecognizeSelector:selector];
}
- (id<MTLEvent>)newEvent {
 NmbEvent *handle=nmb_event_new(gDevice,false);if(!handle)return nil;
 NBEvent *event=[NBEvent new];event.handle=handle;return (id<MTLEvent>)event;
}
- (id<MTLSharedEvent>)newSharedEvent {
 NmbEvent *handle=nmb_event_new(gDevice,true);if(!handle)return nil;
 NBSharedEvent *event=[NBSharedEvent new];event.handle=handle;return (id<MTLSharedEvent>)event;
}
- (id<MTLFence>)newFence {
 NmbFence *handle=nmb_fence_new(gDevice);if(!handle)return nil;
 NBFence *fence=[NBFence new];fence.handle=handle;return (id<MTLFence>)fence;
}
- (id<MTLCommandQueue>)newCommandQueue {
 NmbQueue *handle=nmb_queue_new(gDevice);if(!handle)return nil;
 NBQueue *queue=[NBQueue new];queue.handle=handle;return (id<MTLCommandQueue>)queue;
}
- (id<MTLHeap>)newHeapWithDescriptor:(MTLHeapDescriptor *)d {
 if(!d)return nil;
 NmbHeap *handle=nmb_heap_new(gDevice,d.type,d.size,(uint64_t)d.resourceOptions);
 if(!handle){NBLOG(@"native-metal: heap refused: %@",lastError());return nil;}
 NBHeap *heap=[NBHeap new];heap.handle=handle;heap.options=d.resourceOptions;heap.heapType=d.type;return (id<MTLHeap>)heap;
}
- (id)newResourceGroupFromResources:(const id __unsafe_unretained [])resources count:(NSUInteger)count {
 NBResourceGroup *group=[NBResourceGroup new];
 NSMutableArray *held=[NSMutableArray arrayWithCapacity:count];
 for(NSUInteger i=0;i<count;i++)if(resources[i])[held addObject:resources[i]];
 group.resources=held;return group;
}
// SweetMetal2: a texture over raw memory, and the layout object for such memory (MTLTextureLayout SPI), in one linear layout of
// our choosing that both answer alike: rows of the runtime's required linear pitch, level 0 of a 2D texture, page-aligned
// memory (it is imported, not copied). Clients ask the layout for size and alignment, allocate, then wrap the memory
// (RenderBox's drawables; without these: "RBDisplayList: no drawable" and blank widgets). A caller that never asked
// gets the pitch its length implies where the required one does not fit.
- (id<MTLTexture>)newTextureWithBytesNoCopy:(void *)bytes length:(NSUInteger)length descriptor:(MTLTextureDescriptor *)d deallocator:(void (^)(void *,NSUInteger))deallocator {
 const size_t page=(size_t)getpagesize();
 if(!bytes || !d || !length || ((uintptr_t)bytes&(page-1)) || d.textureType!=MTLTextureType2D || d.mipmapLevelCount>1 || d.arrayLength>1 || d.sampleCount>1){
  NBLOG(@"native-metal: newTextureWithBytesNoCopy refused (%p %lu type %lu levels %lu)",bytes,(unsigned long)length,(unsigned long)d.textureType,(unsigned long)d.mipmapLevelCount);
  return nil;
 }
 uint32_t bw=0,bh=0,bb=0;
 if(sweetBlock(d.pixelFormat,&bw,&bh,&bb)) {
  /* compressed: no linear texture of it exists, so a texture of its own, filled from the caller's blocks (packed rows) */
  const NSUInteger row=(d.width+bw-1)/bw*bb,rows=(d.height+bh-1)/bh;
  if(row*rows>length){NBLOG(@"native-metal: newTextureWithBytesNoCopy: %lux%lu fmt %lu needs %lu bytes, has %lu",(unsigned long)d.width,(unsigned long)d.height,(unsigned long)d.pixelFormat,(unsigned long)(row*rows),(unsigned long)length);return nil;}
  MTLTextureDescriptor *own=[d copy];
  own.storageMode=MTLStorageModeShared;
  id<MTLTexture> texture=[(id<MTLDevice>)self newTextureWithDescriptor:own];
  if(!texture)return nil;
  [texture replaceRegion:MTLRegionMake2D(0,0,d.width,d.height) mipmapLevel:0 withBytes:bytes bytesPerRow:row];
  SweetMemoryOwner *owner=[SweetMemoryOwner new];owner.deallocator=deallocator;owner.bytes=bytes;owner.length=length;
  objc_setAssociatedObject(texture,&sweetMemoryOwnerKey,owner,OBJC_ASSOCIATION_RETAIN);
  return texture;
 }
 uint64_t row=nmb_device_linear_bytes_per_row(gDevice,d.pixelFormat,d.width);
 if(row && row*d.height>length && length%d.height==0)row=length/d.height;
 if(sweetTexelBytes(d.pixelFormat)==1)B29LOG(@"B29 bytesNoCopy %lux%lu fmt %lu length %lu pitch %llu",(unsigned long)d.width,(unsigned long)d.height,(unsigned long)d.pixelFormat,(unsigned long)length,(unsigned long long)row);
 if(!row || row*d.height>length){NBLOG(@"native-metal: newTextureWithBytesNoCopy: %lux%lu fmt %lu does not fit %lu bytes",(unsigned long)d.width,(unsigned long)d.height,(unsigned long)d.pixelFormat,(unsigned long)length);return nil;}
 id<MTLBuffer> buffer=[(id<MTLDevice>)self newBufferWithBytesNoCopy:bytes length:(length+page-1)&~(page-1) options:MTLResourceStorageModeShared deallocator:deallocator];
 if(!buffer)return nil;
 return (id<MTLTexture>)textureOnBuffer((NBBuffer *)buffer,d,0,row,false);
}
- (id)newTextureLayoutWithDescriptor:(MTLTextureDescriptor *)d isHeapOrBufferBacked:(BOOL)backed {
 (void)backed;
 if(!d || d.textureType!=MTLTextureType2D || d.mipmapLevelCount>1 || d.arrayLength>1 || d.sampleCount>1){
  NBLOG(@"native-metal: newTextureLayoutWithDescriptor refused (type %lu levels %lu)",(unsigned long)d.textureType,(unsigned long)d.mipmapLevelCount);
  return nil;
 }
 uint32_t bw=0,bh=0,bb=0;
 if(sweetBlock(d.pixelFormat,&bw,&bh,&bb))
  return [[SweetTextureLayout alloc] initWithBlocks:bw by:bh bytes:bb width:d.width height:d.height];
 const uint64_t row=nmb_device_linear_bytes_per_row(gDevice,d.pixelFormat,d.width);
 if(!row){NBLOG(@"native-metal: newTextureLayoutWithDescriptor: no linear pitch for fmt %lu: %@",(unsigned long)d.pixelFormat,lastError());return nil;}
 return [[SweetTextureLayout alloc] initWithRow:row height:d.height texel:sweetTexelBytes(d.pixelFormat)];
}
- (id<MTLCommandQueue>)newCommandQueueWithMaxCommandBufferCount:(NSUInteger)count {(void)count;return [(id<MTLDevice>)self newCommandQueue];}
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options {
 NmbBuffer *handle=nmb_buffer_new(gDevice,length,options,nullptr);
 if(!handle){NBLOG(@"native-metal: buffer refused: %@",lastError());return nil;}
 NBBuffer *buffer=[NBBuffer new];buffer.handle=handle;buffer.options=options;return (id<MTLBuffer>)buffer;
}
- (id<MTLBuffer>)newBufferWithBytes:(const void *)bytes length:(NSUInteger)length options:(MTLResourceOptions)options {
 if(!bytes)return nil;
 NmbBuffer *handle=nmb_buffer_new(gDevice,length,options,bytes);
 if(!handle){NBLOG(@"native-metal: buffer refused: %@",lastError());return nil;}
 NBBuffer *buffer=[NBBuffer new];buffer.handle=handle;buffer.options=options;return (id<MTLBuffer>)buffer;
}
- (id<MTLBuffer>)newBufferWithBytesNoCopy:(void *)pointer length:(NSUInteger)length options:(MTLResourceOptions)options deallocator:(void (^)(void *,NSUInteger))deallocator {
 NmbBuffer *handle=nmb_buffer_new_no_copy(gDevice,pointer,length,options);
 if(!handle){NBLOG(@"native-metal: no-copy buffer refused: %@",lastError());return nil;}
 NBBuffer *buffer=[NBBuffer new];buffer.handle=handle;buffer.options=options;
 buffer.wrapped=pointer;buffer.deallocator=deallocator;
 return (id<MTLBuffer>)buffer;
}
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d {
 NmbTextureDescriptor descriptor;if(!textureDescriptor(d,&descriptor))return nil;
 NmbTexture *handle=nmb_texture_new(gDevice,&descriptor);noteFormat((uint32_t)descriptor.format,handle!=nullptr);
 if(!handle){NBLOG(@"native-metal: texture refused: %@",lastError());return nil;}
 NBTexture *texture=[NBTexture new];texture.handle=handle;nmb_texture_describe(handle,&descriptor);texture.descriptor=descriptor;return (id<MTLTexture>)texture;
}
// 0112 (camera preview): textures over planar IOSurfaces, after ios-redfin (MIT). YCBCR8_420_2P(_sRGB) (500/501) over a 420v/420f surface
// becomes an RGBA8(_sRGB) copy converted on the CPU (BT.601, video or full range by the fourcc); RGB10A8_2P_XR10(_sRGB) (550/551) a
// BGRA8(_sRGB) copy (XR10 colour from plane 0, alpha from plane 1). Filled again when the surface's seed moved on (checked at every bind).
// Every other planar kind is refused and logged once. Off: M2V_PLANAR=0 (all planar surfaces refused, as before).
static bool planarOn() {static const bool on=[]{const char *p=getenv("M2V_PLANAR");return !p||strcmp(p,"0");}();return on;}
static void planarRefused(OSType fourcc,NSUInteger format,NSUInteger plane) {
 static std::mutex lock;static std::vector<uint64_t> seen;
 const uint64_t key=((uint64_t)fourcc<<32)|((uint64_t)format<<4)|(plane&15);
 {std::lock_guard<std::mutex> hold(lock);if(std::find(seen.begin(),seen.end(),key)!=seen.end())return;seen.push_back(key);}
 NBLOG(@"native-metal: planar iosurface refused: %c%c%c%c fmt %lu plane %lu",(char)(fourcc>>24),(char)(fourcc>>16),(char)(fourcc>>8),(char)fourcc,(unsigned long)format,(unsigned long)plane);
}
static inline uint8_t clamp8(int v) {return (uint8_t)(v<0?0:v>255?255:v);}
static void planarFill(NBTexture *t) {
 IOSurfaceRef s=t.planarSurface;if(!s)return;
 const uint32_t seed=IOSurfaceGetSeed(s);if(seed==t.planarSeed)return;
 const NSUInteger w=t.width,h=t.height;
 if(IOSurfaceLock(s,kIOSurfaceLockReadOnly,nullptr))return;
 const uint8_t *p0=(const uint8_t *)IOSurfaceGetBaseAddressOfPlane(s,0),*p1=(const uint8_t *)IOSurfaceGetBaseAddressOfPlane(s,1);
 const size_t r0=IOSurfaceGetBytesPerRowOfPlane(s,0),r1=IOSurfaceGetBytesPerRowOfPlane(s,1);
 const size_t w0=IOSurfaceGetWidthOfPlane(s,0),h0=IOSurfaceGetHeightOfPlane(s,0),w1=IOSurfaceGetWidthOfPlane(s,1),h1=IOSurfaceGetHeightOfPlane(s,1);
 if(!p0 || !p1 || w>w0 || h>h0 || !w1 || !h1){IOSurfaceUnlock(s,kIOSurfaceLockReadOnly,nullptr);t.planarSeed=seed;return;}
 std::vector<uint8_t> out(w*h*4);
 const NSUInteger format=t.pixelFormat;
 if(format==MTLPixelFormatRGBA8Unorm || format==MTLPixelFormatRGBA8Unorm_sRGB){
  // BT.601, 16.16 fixed point. 420v: Y 16-235, C 16-240; 420f: full range.
  const bool video=IOSurfaceGetPixelFormat(s)=='420v';
  const int ky=video?76309:65536,krv=video?104597:91881,kgu=video?25675:22554,kgv=video?53279:46802,kbu=video?132201:116130,yo=video?16:0;
  for(NSUInteger y=0;y<h;y++){
   const uint8_t *ly=p0+y*r0,*lc=p1+std::min((size_t)(y/2),h1-1)*r1;uint8_t *o=&out[y*w*4];
   for(NSUInteger x=0;x<w;x++,o+=4){
    const size_t cx=std::min((size_t)(x/2),w1-1)*2;
    const int Y=(ly[x]-yo)*ky,U=lc[cx]-128,V=lc[cx+1]-128;
    o[0]=clamp8((Y+krv*V+32768)>>16);o[1]=clamp8((Y-kgu*U-kgv*V+32768)>>16);o[2]=clamp8((Y+kbu*U+32768)>>16);o[3]=255;
   }
  }
 } else {
  // XR10: 10-bit channels in a 32-bit word (B low), 384 = 0.0, 894 = 1.0; alpha 8-bit from plane 1.
  for(NSUInteger y=0;y<h;y++){
   const uint32_t *l=(const uint32_t *)(p0+y*r0);const uint8_t *la=p1+std::min((size_t)y,h1-1)*r1;uint8_t *o=&out[y*w*4];
   for(NSUInteger x=0;x<w;x++,o+=4){
    const uint32_t v=l[x];
    o[0]=clamp8(((int)(v&1023)-384)*255/510);o[1]=clamp8(((int)((v>>10)&1023)-384)*255/510);o[2]=clamp8(((int)((v>>20)&1023)-384)*255/510);
    o[3]=la[std::min((size_t)x,w1-1)];
   }
  }
 }
 IOSurfaceUnlock(s,kIOSurfaceLockReadOnly,nullptr);
 [t replaceRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:0 withBytes:out.data() bytesPerRow:w*4];
 t.planarSeed=seed;
}
// The surface's pages are the texels: its mapping in this process is imported without a copy (pinned by the kernel, mapped into the GPU's
// 4 KiB context by Turnip), so every process that imports the surface renders and samples the same memory.
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d iosurface:(IOSurfaceRef)surface plane:(NSUInteger)plane {
 if(!surface || !d)return nil;
 if(IOSurfaceGetPlaneCount(surface)){
  const OSType fourcc=IOSurfaceGetPixelFormat(surface);const NSUInteger f=d.pixelFormat;
  const bool yuv=(f==500 || f==501) && (fourcc=='420v' || fourcc=='420f'),xr=(f==550 || f==551);
  if(!planarOn() || plane || (!yuv && !xr) || IOSurfaceGetPlaneCount(surface)<2 || d.textureType!=MTLTextureType2D){planarRefused(fourcc,f,plane);return nil;}
  MTLTextureDescriptor *own=[d copy];
  own.pixelFormat=yuv?(f==501?MTLPixelFormatRGBA8Unorm_sRGB:MTLPixelFormatRGBA8Unorm):(f==551?MTLPixelFormatBGRA8Unorm_sRGB:MTLPixelFormatBGRA8Unorm);
  own.storageMode=MTLStorageModeShared;own.usage=MTLTextureUsageShaderRead;own.mipmapLevelCount=1;
  NBTexture *texture=(NBTexture *)[(id<MTLDevice>)self newTextureWithDescriptor:own];
  if(!texture){planarRefused(fourcc,f,plane);return nil;}
  texture.planarSurface=(IOSurfaceRef)CFRetain(surface);texture.planarSeed=IOSurfaceGetSeed(surface)-1;planarFill(texture);
  return (id<MTLTexture>)texture;
 }
 if(plane)return nil;
 void *base=IOSurfaceGetBaseAddress(surface);const size_t page=(size_t)getpagesize(),allocation=IOSurfaceGetAllocSize(surface);
 if(!page || (page&(page-1)) || !base || ((uintptr_t)base&(page-1)) || !allocation || allocation>SIZE_MAX-(page-1))return nil;
 const size_t bytes=(allocation+page-1)&~(page-1);
 NBBuffer *buffer=surfaceBuffer(surface,base,bytes);if(!buffer)return nil;
 const OSType fourcc=IOSurfaceGetPixelFormat(surface);
 if(sweetTexelBytes(d.pixelFormat)==1 || IOSurfaceGetBytesPerElement(surface)==1)
  B29LOG(@"B29 iosurface %lux%lu fmt %lu surface %c%c%c%c %zux%zu bpe %zu bpr %zu plane %lu pitch %zu",(unsigned long)d.width,(unsigned long)d.height,(unsigned long)d.pixelFormat,
   (char)(fourcc>>24),(char)(fourcc>>16),(char)(fourcc>>8),(char)fourcc,IOSurfaceGetWidth(surface),IOSurfaceGetHeight(surface),IOSurfaceGetBytesPerElement(surface),
   IOSurfaceGetBytesPerRow(surface),(unsigned long)plane,IOSurfaceGetBytesPerRow(surface));
 NBTexture *texture=textureOnBuffer(buffer,d,0,IOSurfaceGetBytesPerRow(surface),true);if(!texture)return nil;
 texture.surface=(IOSurfaceRef)CFRetain(surface);return (id<MTLTexture>)texture;
}
- (id<MTLLibrary>)newLibraryWithData:(dispatch_data_t)data error:(NSError **)error {
 const void *bytes=nullptr;size_t size=0;dispatch_data_t mapped=dispatch_data_create_map(data,&bytes,&size);
 const uint64_t libraryBegan=nowUs();
 NmbLibrary *handle=nmb_library_new(gDevice,bytes,size,false);(void)mapped;gTimeLibrary.add(libraryBegan);
 if(!handle)return failWith<id<MTLLibrary>>(error,@"library",nil);
 NBLibrary *library=[NBLibrary new];library.handle=handle;if(error)*error=nil;return (id<MTLLibrary>)library;
}
- (id<MTLLibrary>)newLibraryWithFile:(NSString *)path error:(NSError **)error {
 NSData *bytes=path?[NSData dataWithContentsOfFile:path options:NSDataReadingMappedIfSafe error:error]:nil;if(!bytes)return nil;
 const uint64_t libraryBegan=nowUs();
 NmbLibrary *handle=nmb_library_new(gDevice,bytes.bytes,bytes.length,false);gTimeLibrary.add(libraryBegan);
 if(!handle)return failWith<id<MTLLibrary>>(error,@"library",nil);
 NBLibrary *library=[NBLibrary new];library.handle=handle;if(error)*error=nil;return (id<MTLLibrary>)library;
}
- (id<MTLLibrary>)newLibraryWithURL:(NSURL *)url error:(NSError **)error {
 if(!url.isFileURL){if(error)*error=nbError(@"a file URL is required");return nil;}
 return [(id<MTLDevice>)self newLibraryWithFile:url.path error:error];
}
- (id<MTLLibrary>)newDefaultLibraryWithBundle:(NSBundle *)bundle error:(NSError **)error {
 NSURL *url=[bundle URLForResource:@"default" withExtension:@"metallib"];
 if(!url){if(error)*error=nbError(@"the bundle has no default.metallib");return nil;}
 return [(id<MTLDevice>)self newLibraryWithURL:url error:error];
}
- (id<MTLLibrary>)newDefaultLibrary {return [(id<MTLDevice>)self newDefaultLibraryWithBundle:[NSBundle mainBundle] error:nil];}
- (id<MTLComputePipelineState>)newComputePipelineStateWithFunction:(id<MTLFunction>)function error:(NSError **)error {
 return [(id<MTLDevice>)self newComputePipelineStateWithFunction:function options:MTLPipelineOptionNone reflection:nil error:error];
}
- (id<MTLComputePipelineState>)newComputePipelineStateWithFunction:(id<MTLFunction>)function options:(MTLPipelineOption)options
                                                         reflection:(MTLAutoreleasedComputePipelineReflection *)reflection error:(NSError **)error {
 if(reflection)*reflection=nil;
 if(![(id)function isKindOfClass:[NBFunction class]]){if(error)*error=nbError(@"foreign function");return nil;}
 const NmbPipelineOptions creation={(uint64_t)options,nullptr,0};
 const uint64_t pipelineBegan=nowUs();
 NmbComputePipeline *handle=nmb_compute_pipeline_new_with_options(gDevice,[(NBFunction *)function handle],&creation);gTimePipeline.add(pipelineBegan);
 if(!handle)return failWith<id<MTLComputePipelineState>>(error,@"compute pipeline",nil);
 NBComputePipeline *state=[NBComputePipeline new];state.handle=handle;gPipelines++;if(error)*error=nil;return (id<MTLComputePipelineState>)state;
}
- (id<MTLComputePipelineState>)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)d options:(MTLPipelineOption)options
                                                       reflection:(MTLAutoreleasedComputePipelineReflection *)reflection error:(NSError **)error {
 if(reflection)*reflection=nil;
 NmbComputePipelineDescriptor p={};if(!computeDescriptor(d,&p,error))return nil;
 std::vector<NmbBinaryArchive *> archives;
 if(!pipelineArchives(d.binaryArchives,archives,error))return nil;
 const NmbPipelineOptions creation={(uint64_t)options,archives.data(),archives.size()};
 const uint64_t pipelineBegan=nowUs();
 NmbComputePipeline *handle=nmb_compute_pipeline_new_with_descriptor(gDevice,&p,&creation);gTimePipeline.add(pipelineBegan);
 if(!handle)return failWith<id<MTLComputePipelineState>>(error,@"compute pipeline",nil);
 NBComputePipeline *state=[NBComputePipeline new];state.handle=handle;state.label=d.label;gPipelines++;
 if(error)*error=nil;return (id<MTLComputePipelineState>)state;
}
// 0111 (B24): the async pipeline forms, answered synchronously.
- (void)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d completionHandler:(void (^)(id<MTLRenderPipelineState>,NSError *))handler {
 NSError *error=nil;id<MTLRenderPipelineState> state=[(id<MTLDevice>)self newRenderPipelineStateWithDescriptor:d error:&error];handler(state,error);
}
- (void)newComputePipelineStateWithFunction:(id<MTLFunction>)function completionHandler:(void (^)(id<MTLComputePipelineState>,NSError *))handler {
 NSError *error=nil;id<MTLComputePipelineState> state=[(id<MTLDevice>)self newComputePipelineStateWithFunction:function error:&error];handler(state,error);
}
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d error:(NSError **)error {
 return [(id<MTLDevice>)self newRenderPipelineStateWithDescriptor:d options:MTLPipelineOptionNone reflection:nil error:error];
}
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d options:(MTLPipelineOption)options
                                                        reflection:(MTLAutoreleasedRenderPipelineReflection *)reflection error:(NSError **)error {
 if(reflection)*reflection=nil;
 auto *p=new NmbRenderPipelineDescriptor();
 if(!renderDescriptor(d,p,error)){delete p;return nil;}
 std::vector<NmbBinaryArchive *> archives;
 if(!pipelineArchives(d.binaryArchives,archives,error)){delete p;return nil;}
 const NmbPipelineOptions creation={(uint64_t)options,archives.data(),archives.size()};
 const uint64_t pipelineBegan=nowUs();const NBCost costBegan=NBCost::now();
 NmbRenderPipeline *handle=nmb_render_pipeline_new_with_options(gDevice,p,&creation);delete p;gTimePipeline.add(pipelineBegan);
 journalRenderPipeline(d,pipelineBegan,handle?nullptr:nmb_last_error(),gJournalPath.empty()?std::string():NBCost::now().since(costBegan));
 if(!handle)return failWith<id<MTLRenderPipelineState>>(error,@"render pipeline",nil);
 NBRenderPipeline *state=[NBRenderPipeline new];state.handle=handle;gPipelines++;if(error)*error=nil;
 if(drawLogPath()||cullEnabled()||deadfillMode()) {   // sweet drawlog / sweet cull / sweet deadfill
  state.label=d.label;
  if([(id)d.vertexFunction isKindOfClass:[NBFunction class]])state.vertexName=((NBFunction *)d.vertexFunction).name.UTF8String?:"";
  if([(id)d.fragmentFunction isKindOfClass:[NBFunction class]])state.fragmentName=((NBFunction *)d.fragmentFunction).name.UTF8String?:"";
  state.blend=d.colorAttachments[0].blendingEnabled;state.writeMask=d.colorAttachments[0].writeMask;
  {unsigned m=0;for(unsigned i=0;i<8;i++){MTLRenderPipelineColorAttachmentDescriptor *a=d.colorAttachments[i];if(a.pixelFormat!=MTLPixelFormatInvalid&&!a.blendingEnabled&&a.writeMask==MTLColorWriteMaskAll)m|=1u<<i;}state.opaqueMask=m;
   unsigned w=0;for(unsigned i=0;i<8;i++){MTLRenderPipelineColorAttachmentDescriptor *a=d.colorAttachments[i];if(a.pixelFormat!=MTLPixelFormatInvalid&&a.writeMask!=MTLColorWriteMaskNone)w|=1u<<i;}state.writtenMask=w;}
  if(MTLVertexDescriptor *vd=d.vertexDescriptor) {   // sweet cull: the position layout of vertex buffer 1
   state.vstride=vd.layouts[1].stride;
   for(unsigned i=0;i<31;i++){MTLVertexAttributeDescriptor *a=vd.attributes[i];if(a.bufferIndex==1&&a.offset==0&&a.format==MTLVertexFormatFloat4)state.vpos0=YES;}
   MTLVertexAttributeDescriptor *a0=vd.attributes[0];   // sweet deadfill
   if(a0.format!=MTLVertexFormatInvalid&&a0.bufferIndex<8) {
    state.dfPosFmt=(unsigned)a0.format;state.dfPosOff=(unsigned)a0.offset;state.dfPosBuf=(unsigned)a0.bufferIndex;state.dfStride=(unsigned)vd.layouts[a0.bufferIndex].stride;
    for(unsigned k=1;k<31;k++){MTLVertexAttributeDescriptor *c=vd.attributes[k];
     if(c.bufferIndex==a0.bufferIndex&&(c.format==MTLVertexFormatHalf4||c.format==MTLVertexFormatFloat4||c.format==MTLVertexFormatUChar4Normalized)){state.dfColFmt=(unsigned)c.format;state.dfColOff=(unsigned)c.offset;break;}}
   }
  }
  state.dfSamples=(unsigned)d.rasterSampleCount;state.dfA2C=d.alphaToCoverageEnabled;   // sweet deadfill
 }
 return (id<MTLRenderPipelineState>)state;
}
- (id<MTLBinaryArchive>)newBinaryArchiveWithDescriptor:(MTLBinaryArchiveDescriptor *)d error:(NSError **)error {
 if(d.url && !d.url.isFileURL){if(error)*error=nbError(@"a file URL is required");return nil;}
 NmbBinaryArchive *handle=nmb_binary_archive_new(gDevice,d.url?d.url.fileSystemRepresentation:nullptr);
 if(!handle)return failWith<id<MTLBinaryArchive>>(error,@"binary archive",nil);
 NBBinaryArchive *archive=[NBBinaryArchive new];archive.handle=handle;if(error)*error=nil;return (id<MTLBinaryArchive>)archive;
}
- (NSUInteger)requiredLinearTextureBytesPerRowForDescriptor:(MTLTextureDescriptor *)d {
 const uint64_t row=nmb_device_linear_bytes_per_row(gDevice,d.pixelFormat,d.width);
 if(!row)NBLOG(@"native-metal: requiredLinearTextureBytesPerRow: %@",lastError());
 return row;
}
- (id<MTLSamplerState>)newSamplerStateWithDescriptor:(MTLSamplerDescriptor *)d {
 NmbSamplerDescriptor s={(uint64_t)d.minFilter,(uint64_t)d.magFilter,(uint64_t)d.mipFilter,(uint64_t)d.sAddressMode,(uint64_t)d.tAddressMode,(uint64_t)d.rAddressMode,
     (uint64_t)d.borderColor,(uint64_t)d.normalizedCoordinates,d.maxAnisotropy,(uint64_t)d.compareFunction,(uint64_t)d.lodAverage,d.lodMinClamp,d.lodMaxClamp};
 NmbSampler *handle=nmb_sampler_new(gDevice,&s);
 if(!handle){NBLOG(@"native-metal: sampler refused: %@",lastError());return nil;}
 NBSampler *sampler=[NBSampler new];sampler.handle=handle;return (id<MTLSamplerState>)sampler;
}
- (id<MTLDepthStencilState>)newDepthStencilStateWithDescriptor:(MTLDepthStencilDescriptor *)d {
 auto face=[](MTLStencilDescriptor *s)->NmbStencil {
  return {(uint64_t)s.stencilCompareFunction,(uint64_t)s.stencilFailureOperation,(uint64_t)s.depthFailureOperation,(uint64_t)s.depthStencilPassOperation,s.readMask,s.writeMask};
 };
 NmbDepthStencilDescriptor s={(uint64_t)d.depthCompareFunction,(uint64_t)d.depthWriteEnabled,face(d.frontFaceStencil),face(d.backFaceStencil)};
 NmbDepthStencil *handle=nmb_depth_stencil_new(gDevice,&s);
 if(!handle){NBLOG(@"native-metal: depth/stencil state refused: %@",lastError());return nil;}
 NBDepthStencil *state=[NBDepthStencil new];state.handle=handle;return (id<MTLDepthStencilState>)state;
}
@end

// The device queries the runtime answers (nmb_device_answer_selector), as methods of the device class. One without an argument is read once
// and installed only where the device has an answer of its own; one with an argument asks the runtime each call, and a refusal answers 0/NO
// with the reason logged.
static std::unordered_map<SEL,uint64_t> gAnswers;
static NSUInteger countAnswer(id,SEL cmd) {return (NSUInteger)gAnswers.at(cmd);}
static BOOL flagAnswer(id,SEL cmd) {return gAnswers.at(cmd)!=0;}
static uint64_t askAnswer(SEL cmd,NSUInteger arg) {
 uint64_t value=0;
 if(!nmb_device_answer(gDevice,sel_getName(cmd),arg,&value)){NBLOG(@"native-metal: %s(%lu): %@",sel_getName(cmd),(unsigned long)arg,lastError());return 0;}
 return value;
}
static NSUInteger countArgAnswer(id,SEL cmd,NSUInteger arg) {return (NSUInteger)askAnswer(cmd,arg);}
static BOOL flagArgAnswer(id,SEL cmd,NSUInteger arg) {return askAnswer(cmd,arg)!=0;}
static void installAnswers(Class cls) {
 uint32_t kind=0;
 for(size_t i=0;const char *name=nmb_device_answer_selector(i,&kind);i++) {
  SEL sel=sel_registerName(name);uint64_t value=0;
  switch(kind) {
  case NMB_ANSWER_COUNT: case NMB_ANSWER_FLAG:
   if(!nmb_device_answer(gDevice,name,0,&value)){NBLOG(@"native-metal: no device answer: %@",lastError());continue;}
   // Extended-range 10-bit (XR10) framebuffers are withheld: this display chain presents 8-bit BGRX scanout only. The render server picks its
   // wide-gamut 64-bit (and Apple-compressed) framebuffer when a device offers them, and the display driver could not show it (int-089: the
   // swap's surface was '&w4a', 8 bytes per pixel, and every swap was refused).
   if(!strcmp(name,"supportsExtendedXR10Formats"))value=0;
   gAnswers[sel]=value;
   class_addMethod(cls,sel,kind==NMB_ANSWER_COUNT?(IMP)countAnswer:(IMP)flagAnswer,kind==NMB_ANSWER_COUNT?"Q16@0:8":"B16@0:8");
   break;
  case NMB_ANSWER_COUNT_ARG: class_addMethod(cls,sel,(IMP)countArgAnswer,"Q24@0:8Q16");break;
  case NMB_ANSWER_FLAG_ARG: class_addMethod(cls,sel,(IMP)flagArgAnswer,"B24@0:8Q16");break;
  }
 }
}

extern "C" id native_bridge_metal_open(const char *loader,const char *cache,uint64_t registryID,void (*missing)(const char *,const char *)) {
 if(gDeviceObject)return gDeviceObject;
 gMissing=missing;gInFlight=[NSMutableSet set];
 gDevice=nmb_device_open(loader,cache);
 if(!gDevice){NBLOG(@"native-metal: device refused: %s",nmb_last_error());return nil;}
 if(cache){gJournalPath=std::string(cache)+"/pipelines.log";gJournalBegan=nowUs();}
 char name[128]={};nmb_device_name(gDevice,name,sizeof name);gName=[NSString stringWithUTF8String:name];gRegistryID=registryID;
 // The target families supportsFamily: answers true, with the rows of Apple's tables the device does not meet in them (nmb_device_family_waivers).
 {char waived[4096]={};nmb_device_family_waivers(gDevice,waived,sizeof waived);
  for(char *line=waived,*next;*line;line=next){next=strchr(line,'\n');if(next)*next++=0;else next=line+strlen(line);journal(std::string("F ")+line);}}
 Class base=objc_getClass("_MTLDevice");
#if !TARGET_OS_IPHONE
 // Host instrument: on NSObject every selector a client sends that this file lacks reaches the missing hook, instead of the host's
 // own _MTLDevice implementation.
 if(getenv("NMB_DEVICE_BASE_NSOBJECT"))base=[NSObject class];
#endif
#if TARGET_OS_IPHONE
 // The dynamic subclass adds no instance variables, so any base size works (iPadOS 17.7.11: 536 bytes).
 if(!base){NBLOG(@"native-metal: no _MTLDevice");return nil;}
 NBLOG(@"native-metal: _MTLDevice is %zu bytes",class_getInstanceSize(base));
#endif
 if(!base)base=[NSObject class];
 Class cls=objc_allocateClassPair(base,"NativeBridgeMetalDevice",0);if(!cls)return nil;
 unsigned count=0;Method *methods=class_copyMethodList([NBDeviceMethods class],&count);
 for(unsigned i=0;i<count;i++)class_addMethod(cls,method_getName(methods[i]),method_getImplementation(methods[i]),method_getTypeEncoding(methods[i]));
 free(methods);installAnswers(cls);class_addProtocol(cls,@protocol(MTLDevice));
 // The original registration (_MTLAddDevice) asserts that a plugin's device conforms to Metal's private device protocol as well; the target's own
 // drivers get it from their IOGPU base class. Conformance only answers conformsToProtocol:, the methods are still looked up one by one.
 if(Protocol *spi=objc_getProtocol("MTLDeviceSPI"))class_addProtocol(cls,spi);
 // SweetMetal2: Metal checks textures against its private implementation protocol too, and NSLogs "texture is not a
 // MTLTextureImplementation." on every miss (~600 lines/s from backboardd while scrolling).
 {Protocol *tp=objc_getProtocol("MTLTextureImplementation");if(tp)class_addProtocol([NBTexture class],tp);
  NBLOG(@"native-metal: MTLTextureImplementation %s",tp?([NBTexture conformsToProtocol:tp]?"adopted":"not adopted"):"missing");}
 // SweetMetal2: MTLRenderPipelineDescriptor's setVertexFunction:/setFragmentFunction: NSLog "... is not a MTLFunction"
 // on every pipeline whose function doesn't conform (harmless: the function is stored anyway), so declare the protocols.
 class_addProtocol([NBFunction class],@protocol(MTLFunction));class_addProtocol([NBLibrary class],@protocol(MTLLibrary));
 objc_registerClassPair(cls);
 gDeviceObject=[cls new];
 return gDeviceObject;
}

// Bare AIR bitcode (no metallib wrapper), which Metal itself refuses: for the bridge's own fixtures only.
extern "C" id native_bridge_metal_library_with_air(const void *bytes,size_t length) {
 NmbLibrary *handle=gDevice?nmb_library_new(gDevice,bytes,length,true):nullptr;if(!handle)return nil;
 NBLibrary *library=[NBLibrary new];library.handle=handle;return library;
}
extern "C" bool native_bridge_metal_shader_cache_stats(NmbShaderCacheStats *out) {
 return gDevice && nmb_device_shader_cache_stats(gDevice,out);
}
extern "C" bool native_bridge_metal_work_stats(uint64_t out[10]) {
 return gDevice && nmb_device_work_stats(gDevice,out);
}

extern "C" size_t native_bridge_metal_status(char *out,size_t size) {
 std::lock_guard<std::mutex> lock(gProblemLock);
 return (size_t)snprintf(out,size,"c%llu/%llu d%llu p%llu x%llu R%llu D%llu F%llu M%llu C%llu B%llu %s | %s",(unsigned long long)gCommits.load(),(unsigned long long)gCommitFailures.load(),(unsigned long long)gCompleted.load(),
  (unsigned long long)gPipelines.load(),(unsigned long long)gProblems.load(),(unsigned long long)gPasses.load(),(unsigned long long)gDraws.load(),(unsigned long long)gFullScreenPasses.load(),(unsigned long long)(gBiggestPass.load()>>10),(unsigned long long)gDispatches.load(),(unsigned long long)gBlits.load(),gLastProblems[0],gLastProblems[1]);
}

static size_t native_bridge_metal_formats_locked(char *out,size_t size) {
 int n=snprintf(out,size,"tex x%llu f",(unsigned long long)gTextureRefusals.load());
 for(unsigned i=0;i<gFormatCount && n>0 && (size_t)n<size;i++)n+=snprintf(out+n,size-(size_t)n,"%u ",gFormats[i]);
 return n>0?(size_t)n:0;
}
// The benchmark's rows (native_bridge_metal_benchmark), kept for the telemetry table.
static std::mutex gBenchLock;
static char gBench[4][96];
static std::atomic<unsigned> gBenchRuns;
extern "C" size_t native_bridge_metal_benchmark(id device,const char *metallib,unsigned pairs,char *out,size_t size) {
 auto now=[]{return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);};
 // the first run fills rows 0-1 (Y, Y1), a later one rows 2-3 (Z, Z1), so a busy and an idle run are both kept
 const unsigned base=gBenchRuns++?2u:0u;
 auto put=[&](unsigned row,const char *format,auto... args){std::lock_guard<std::mutex> lock(gBenchLock);snprintf(gBench[base+row],sizeof gBench[base+row],format,args...);
  if(base)gBench[base+row][0]='Z';};
 // compute: a dependent integer chain (no memory), 100M steps
 uint64_t t=now(),x=0x9e3779b97f4a7c15ull;
 for(uint32_t i=0;i<100000000u;i++){x^=x<<13;x^=x>>7;x^=x<<17;}
 const uint64_t computeUs=(now()-t)/1000;
 // first touch: 32 MiB of fresh anonymous memory (the tablet's free memory is small), one store per 4 KiB page (the kernel's fault and zero-fill), then four memsets of it
 const size_t bytes=32u<<20;
 uint64_t faultUs=0,fillUs=0,shaUs=0;
 void *m=mmap(nullptr,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
 if(m!=MAP_FAILED) {
  volatile uint8_t *b=(volatile uint8_t *)m;
  t=now();for(size_t at=0;at<bytes;at+=4096)b[at]=1;faultUs=(now()-t)/1000;
  t=now();for(int k=0;k<4;k++)memset(m,k,bytes);fillUs=(now()-t)/1000;
  unsigned char digest[CC_SHA256_DIGEST_LENGTH];
  t=now();CC_SHA256(m,(CC_LONG)(16u<<20),digest);shaUs=(now()-t)/1000;
  x^=digest[0];
  munmap(m,bytes);
 }
 put(0,"Y cpu=%llums(100M) fault=%lluus/32MiB=%lluns/pg fill=%lluMB/s sha=%lluMB/s %llx",(unsigned long long)(computeUs/1000),
  (unsigned long long)faultUs,(unsigned long long)(faultUs*1000/8192),(unsigned long long)(fillUs?4ull*32*1000000/fillUs:0),
  (unsigned long long)(shaUs?16ull*1000000/shaUs:0),(unsigned long long)(x&0xf));
 // pipelines: the host tool's pairs, in its order
 uint64_t made=0,refused=0,createUs=0,createMax=0,drawUs=0,drawMax=0,libraryUs=0;
 @autoreleasepool {
  id<MTLDevice> dev=(id<MTLDevice>)device;NSError *error=nil;
  t=now();
  id<MTLLibrary> qc=metallib?[dev newLibraryWithURL:[NSURL fileURLWithPath:@(metallib)] error:&error]:nil;
  libraryUs=(now()-t)/1000;
  if(qc) {
   NSArray<NSString *> *names=qc.functionNames;
   id<MTLCommandQueue> queue=[dev newCommandQueue];
   MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64 height:64 mipmapped:NO];
   td.usage=MTLTextureUsageRenderTarget;
   id<MTLTexture> target=[dev newTextureWithDescriptor:td];
   id<MTLBuffer> zeros=[dev newBufferWithLength:65536 options:MTLResourceStorageModeShared];
   unsigned n=0;
   for(NSString *name in names) {
    if(n>=pairs)break;
    id<MTLFunction> f=[qc newFunctionWithName:name];
    if(f.functionType!=MTLFunctionTypeVertex)continue;
    NSString *fragmentName=[name stringByReplacingOccurrencesOfString:@"_vert_" withString:@"_frag_"];
    if([fragmentName isEqualToString:name] || ![names containsObject:fragmentName])continue;
    ++n;
    MTLRenderPipelineDescriptor *d=[MTLRenderPipelineDescriptor new];
    d.vertexFunction=f;d.fragmentFunction=[qc newFunctionWithName:fragmentName];
    d.colorAttachments[0].pixelFormat=MTLPixelFormatBGRA8Unorm;
    t=now();
    id<MTLRenderPipelineState> state=[dev newRenderPipelineStateWithDescriptor:d error:&error];
    const uint64_t took=(now()-t)/1000;
    if(!state){++refused;continue;}
    ++made;createUs+=took;createMax=std::max(createMax,took);
    t=now();
    MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture=target;pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
    id<MTLCommandBuffer> commands=[queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder=[commands renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:state];
    for(NSUInteger i=0;i<8;++i){[encoder setVertexBuffer:zeros offset:0 atIndex:i];[encoder setFragmentBuffer:zeros offset:0 atIndex:i];}
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];[commands commit];[commands waitUntilCompleted];
    const uint64_t drew=(now()-t)/1000;
    drawUs+=drew;drawMax=std::max(drawMax,drew);
   }
  }
 }
 put(1,"Y1 qc lib=%llums made=%llu refused=%llu create=%llums max=%llums draw=%llums max=%llums",(unsigned long long)(libraryUs/1000),
  (unsigned long long)made,(unsigned long long)refused,(unsigned long long)(createUs/1000),(unsigned long long)(createMax/1000),
  (unsigned long long)(drawUs/1000),(unsigned long long)(drawMax/1000));
 size_t at=0;
 if(out && size) {
  std::lock_guard<std::mutex> lock(gBenchLock);
  out[0]=0;for(unsigned r=base;r<base+2;r++)at+=(size_t)snprintf(out+at,size>at?size-at:0,"%s\n",gBench[r]);
 }
 return at;
}
// The telemetry table the plugin streams to the kernel's log: newline-separated rows of at most 90 characters.
extern "C" size_t native_bridge_metal_telemetry(char *out,size_t size) {
 size_t at=0;out[0]=0;
 auto row=[&](const char *format,auto... args){
  char text[200];int n=snprintf(text,sizeof text,format,args...);if(n<=0)return;if(n>90)n=90;
  if(at+(size_t)n+2>=size)return;memcpy(out+at,text,(size_t)n);at+=(size_t)n;out[at++]='\n';out[at]=0;};
 auto ms=[](uint64_t us){return (unsigned long long)(us/1000);};
 {std::lock_guard<std::mutex> lock(gBenchLock);for(unsigned r=0;r<4;r++)if(gBench[r][0])row("%.90s",gBench[r]);}
 row("S c%llu/%llu d%llu p%llu x%llu R%llu D%llu F%llu M%lluk C%llu B%llu",(unsigned long long)gCommits.load(),(unsigned long long)gCommitFailures.load(),
  (unsigned long long)gCompleted.load(),(unsigned long long)gPipelines.load(),(unsigned long long)gProblems.load(),(unsigned long long)gPasses.load(),
  (unsigned long long)gDraws.load(),(unsigned long long)gFullScreenPasses.load(),(unsigned long long)(gBiggestPass.load()>>10),
  (unsigned long long)gDispatches.load(),(unsigned long long)gBlits.load());
 {static uint64_t lastUs,lastDrop,lastClear;const uint64_t now=nowUs(),dr=gDfDropped.load(),cl=gDfCleared.load();   // sweet deadfill
  const double sec=lastUs&&now>lastUs?(now-lastUs)/1e6:0;
  row("DF mode%d dropped %llu cleared %llu held %llu | /s dropped %.1f cleared %.1f",deadfillMode(),(unsigned long long)dr,(unsigned long long)cl,
   (unsigned long long)gDfPasses.load(),sec>0?(dr-lastDrop)/sec:0.,sec>0?(cl-lastClear)/sec:0.);
  lastUs=now;lastDrop=dr;lastClear=cl;}
 row("T pipeline n=%llu total=%llums max=%llums",(unsigned long long)gTimePipeline.count.load(),ms(gTimePipeline.total.load()),ms(gTimePipeline.slowest.load()));
 row("T library n=%llu total=%llums max=%llums",(unsigned long long)gTimeLibrary.count.load(),ms(gTimeLibrary.total.load()),ms(gTimeLibrary.slowest.load()));
 row("T function n=%llu total=%llums max=%llums",
  (unsigned long long)gTimeFunction.count.load(),ms(gTimeFunction.total.load()),ms(gTimeFunction.slowest.load()));
 row("G commit-to-complete n=%llu total=%llums max=%llums mean=%lluus",(unsigned long long)gTimeEncodeToCommit.count.load(),ms(gTimeEncodeToCommit.total.load()),
  ms(gTimeEncodeToCommit.slowest.load()),(unsigned long long)(gTimeEncodeToCommit.count.load()?gTimeEncodeToCommit.total.load()/gTimeEncodeToCommit.count.load():0));
 row("J scheduled wait n=%llu total=%llums max=%llums",(unsigned long long)gTimeWaitScheduled.count.load(),ms(gTimeWaitScheduled.total.load()),ms(gTimeWaitScheduled.slowest.load()));
 row("J completed wait n=%llu total=%llums max=%llums",(unsigned long long)gTimeWaitCompleted.count.load(),ms(gTimeWaitCompleted.total.load()),ms(gTimeWaitCompleted.slowest.load()));
 row("J resource wait n=%llu total=%llums max=%llums",(unsigned long long)gTimeResourceWait.count.load(),ms(gTimeResourceWait.total.load()),ms(gTimeResourceWait.slowest.load()));
 row("J resource checks=%llu skipped=%llu",(unsigned long long)gResourceChecks.load(),(unsigned long long)gResourceSkipped.load());
 row("J surface cache n=%llu bytes=%llu dup=%llu us=%llu max=%llu",(unsigned long long)gTimeSurfaceFlush.count.load(),
  (unsigned long long)gSurfaceFlushBytes.load(),(unsigned long long)gSurfaceFlushDuplicates.load(),
  (unsigned long long)gTimeSurfaceFlush.total.load(),(unsigned long long)gTimeSurfaceFlush.slowest.load());
 // The runtime's VkPipeline makings (the creation log, METAL2VULKAN_LOG_PIPELINES; they happen at draws, not at the state's creation) and
 // its shader cache: where the driver's compile time goes.
 {uint64_t cp[3]={};nmb_command_pool_stats(cp); // bridge 0105
  row("cp made %llu kept %llu capped %llu",(unsigned long long)cp[0],(unsigned long long)cp[1],(unsigned long long)cp[2]);}
 uint64_t work[10]={};
 if(gDevice && nmb_device_work_stats(gDevice,work)) {
  row("V vkpipe n=%llu total=%llums max=%llums waited=%llums failed=%llu",
   (unsigned long long)work[0],ms(work[1]),ms(work[2]),ms(work[3]),(unsigned long long)work[4]);
  row("V cache hit%llu miss%llu wr%llu | msg i%llu w%llu",(unsigned long long)work[7],(unsigned long long)work[8],(unsigned long long)work[9],(unsigned long long)work[5],
   (unsigned long long)work[6]);
  char warning[200];size_t n=nmb_last_warning(warning,sizeof warning);
  if(n){row("W %.86s",warning);if(n>86)row("Wb %.86s",warning+86);}
 }
 {
  std::lock_guard<std::mutex> lock(gProblemLock);
  char formats[160];native_bridge_metal_formats_locked(formats,sizeof formats);row("F %.86s",formats);
  row("P0 %.86s",gLastProblems[0]);row("P0b %.86s",strlen(gLastProblems[0])>86?gLastProblems[0]+86:"");
  row("P1 %.86s",gLastProblems[1]);row("P1b %.86s",strlen(gLastProblems[1])>86?gLastProblems[1]+86:"");
 }
 {
  std::lock_guard<std::mutex> lock(gCommitLock);
  const uint32_t newest=(uint32_t)gCommits.load();
  row("%s","Q timing v2: per-buffer API counts; ctc=commit-to-callback; age includes caller idle");
  for(uint32_t k=0;k<20 && k<newest;k++) {
   const CommitRecord &r=gCommitRing[(newest-k)%32];
   if(r.index!=newest-k)continue;
   row("K%u t=%llu.%03llus rp%u dr%u kpx%llu cs%u bl%u ctc=%lluus",r.index,(unsigned long long)(r.committedUs/1000000),(unsigned long long)((r.committedUs/1000)%1000),
    r.work.passes,r.work.draws,(unsigned long long)(r.work.pixels>>10),r.work.dispatches,r.work.blits,(unsigned long long)r.callbackUs);
   if(k<4)row("E%u age=%lluus scheduled=%lldus",r.index,(unsigned long long)r.ageUs,(long long)r.scheduledUs);
  }
 }
 return at;
}

extern "C" size_t native_bridge_metal_formats(char *out,size_t size) {
 std::lock_guard<std::mutex> lock(gProblemLock);
 return native_bridge_metal_formats_locked(out,size);
}
