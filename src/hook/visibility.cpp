#include "support.h"
#include "visibility_math.h"
#include <atomic>
#include <memory>

namespace acah {
namespace {
using Getter=void (__thiscall*)(void*,Matrix*);
Getter nativeGetter=nullptr;
std::shared_ptr<Mapping> channel;
uintptr_t imageBase=0;
std::atomic<uint32_t> calls{0},expanded{0},boxes{0},rejected{0},lastReport{0};

void __fastcall headFrustum(void* camera,void*,Matrix* output) {
    nativeGetter(camera,output);
    // Only the profiled renderer camera state, with the game's projection
    // aspect below. In particular, square shadow projections are excluded.
    // No native cached matrix is changed.
    if(reinterpret_cast<uintptr_t>(camera)!=imageBase+0xe01e00 || !output || !channel)return;
    ++calls;
    if(!config().headAwareCulling || !hostAlive(channel->get()))return;
    Tracking tracking{};
    if(!readTracking(channel->get(),tracking) || !tracking.valid || !tracking.stereoRequested)return;
    Matrix projection{},second{};
    auto source=reinterpret_cast<const void*>(imageBase+0xe01e70);
    std::memcpy(&projection,source,sizeof(projection));MemoryBarrier();std::memcpy(&second,source,sizeof(second));
    // Thousands of object tests can ask for the same camera planes. Cache the
    // expansion per thread/pose/native matrix, rather than repeat eye inverses.
    struct Cache {Matrix projection{},native{},updated{};uint32_t serial=0,generation=0;bool present=false,valid=false,box=false;};
    thread_local Cache cache;
    if(std::memcmp(&projection,&second,sizeof(projection)) || !perspective(projection) ||
        std::abs(projection.m[5]/projection.m[0]-gameAspect())>0.03f)++rejected;
    else {
        if(!cache.present || cache.serial!=tracking.serial || cache.generation!=tracking.originGeneration ||
            std::memcmp(&cache.projection,&projection,sizeof(projection)) || std::memcmp(&cache.native,output,sizeof(*output))) {
            cache.projection=projection;cache.native=*output;cache.serial=tracking.serial;cache.generation=tracking.originGeneration;cache.present=true;
            cache.valid=visibilityUnion(tracking,projection,*output,config().units,cache.updated,cache.box);
        }
        if(cache.valid) {*output=cache.updated;++expanded;if(cache.box)++boxes;}else ++rejected;
    }
    uint32_t now=GetTickCount(),previous=lastReport.load();
    if(now-previous>3000 && lastReport.compare_exchange_strong(previous,now))
        log("native visibility mainCalls=%u expanded=%u wideHeadBoxes=%u rejected=%u",calls.load(),expanded.load(),boxes.load(),rejected.load());
}
}
std::shared_ptr<Mapping> gameMapping() {
    // Visibility and stereo share one x86 address-space view of the image ring.
    static auto mapping=std::make_shared<Mapping>();return mapping;
}
void initializeVisibility() {
    // Called only for the exact SHA-256 profile, before device creation. This
    // getter copies the main camera's four world side planes to the caller.
    imageBase=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto entry=reinterpret_cast<uint8_t*>(imageBase+0x56c170);
    constexpr uint8_t expected[]={0x55,0x8b,0xec,0x53,0x56}; // four complete instructions
    if(std::memcmp(entry,expected,sizeof(expected))) {log("native visibility profile instruction mismatch; hook disabled");return;}
    try {channel=gameMapping();}catch(const std::exception& e){log("native visibility IPC unavailable: %s",e.what());return;}
    auto trampoline=static_cast<uint8_t*>(VirtualAlloc(nullptr,10,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    if(!trampoline) {log("native visibility trampoline allocation failed");return;}
    std::memcpy(trampoline,expected,5);trampoline[5]=0xe9;
    int32_t resume=static_cast<int32_t>(reinterpret_cast<uintptr_t>(entry+5)-reinterpret_cast<uintptr_t>(trampoline+10));
    std::memcpy(trampoline+6,&resume,4);
    DWORD old=0;
    if(!VirtualProtect(trampoline,10,PAGE_EXECUTE_READ,&old)) {VirtualFree(trampoline,0,MEM_RELEASE);return;}
    FlushInstructionCache(GetCurrentProcess(),trampoline,10);
    if(!VirtualProtect(entry,5,PAGE_EXECUTE_READWRITE,&old)) {VirtualFree(trampoline,0,MEM_RELEASE);return;}
    nativeGetter=reinterpret_cast<Getter>(trampoline);
    uint8_t patch[5]={0xe9};
    int32_t branch=static_cast<int32_t>(reinterpret_cast<uintptr_t>(&headFrustum)-reinterpret_cast<uintptr_t>(entry+5));
    std::memcpy(patch+1,&branch,4);std::memcpy(entry,patch,5);
    FlushInstructionCache(GetCurrentProcess(),entry,5);
    DWORD ignored=0;VirtualProtect(entry,5,old,&ignored);
    log("native visibility getter installed RVA=0056c170 main camera RVA=00e01e00 enabled=%d",config().headAwareCulling);
}
}
