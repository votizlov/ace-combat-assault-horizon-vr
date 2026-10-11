#include "support.h"
#include <unordered_map>
#include <bcrypt.h>
#include <array>
#include <vector>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <share.h>

namespace acah {
namespace {
Config settings;
std::mutex logMutex;
std::once_flag initialization;
FILE* logFile=nullptr;
bool known=false;
std::wstring shaderDumpDirectory;
std::atomic<float> aspect{16.0f/9.0f};
ProjectionObservation observed;
std::mutex projectionMutex;
struct Target { const char* name; uint32_t nameRva; std::vector<uintptr_t> vtables; };
std::array<Target,5> targets{{
    {"CAce7CameraCockpit",0x8106d0,{}}, {"CAce7CameraImpl_Gunner",0x810c78,{}},
    {"CAce7ObjPlayerGunnerHeli",0x80c29c,{}}, {"CAce7ObjEquipment_GatlingGun",0x80ce98,{}},
    {"CAce7CameraManager",0x81004c,{}}
}};
using MultiplyFn=Matrix* (WINAPI*)(Matrix*,const Matrix*,const Matrix*);
MultiplyFn originalMultiply=nullptr;

bool readable(uintptr_t address, size_t bytes) {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) || info.State!=MEM_COMMIT ||
        (info.Protect & (PAGE_NOACCESS|PAGE_GUARD))) return false;
    auto end=reinterpret_cast<uintptr_t>(info.BaseAddress)+info.RegionSize;
    return address <= end && bytes <= end-address;
}
std::string executableHash() {
    wchar_t name[32768]{}; GetModuleFileNameW(nullptr,name,32768);
    std::ifstream file(name,std::ios::binary);
    if (!file) return {};
    BCRYPT_ALG_HANDLE algorithm=nullptr; BCRYPT_HASH_HANDLE hash=nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0) return {};
    DWORD size=0, returned=0;
    if (BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&size),sizeof(size),&returned,0)<0) {
        BCryptCloseAlgorithmProvider(algorithm,0); return {};
    }
    std::vector<unsigned char> object(size); std::array<unsigned char,32> digest{};
    bool ok=BCryptCreateHash(algorithm,&hash,object.data(),size,nullptr,0,0)>=0;
    std::array<char,65536> buffer{};
    while (ok && file) {
        file.read(buffer.data(),buffer.size()); auto read=file.gcount();
        if (read) ok=BCryptHashData(hash,reinterpret_cast<PUCHAR>(buffer.data()),static_cast<ULONG>(read),0)>=0;
    }
    ok=ok && file.eof() && BCryptFinishHash(hash,digest.data(),digest.size(),0)>=0;
    if (hash) BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm,0);
    if (!ok) return {};
    char hex[65]{}; for(size_t i=0;i<digest.size();++i) sprintf_s(hex+2*i,3,"%02x",digest[i]);
    return hex;
}
Matrix* WINAPI hookedMultiply(Matrix* out,const Matrix* a,const Matrix* b) {
    // Copy inputs before a possibly aliasing destination is overwritten.
    Matrix left=*a,right=*b;
    auto result=originalMultiply(out,a,b);
    observeProjection(right,"D3DXMatrixMultiply rhs");
    observeProjection(left,"D3DXMatrixMultiply lhs");
    return result;
}
void patchMultiplyImport() {
    auto base=reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt=reinterpret_cast<IMAGE_NT_HEADERS32*>(base+dos->e_lfanew);
    auto rva=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva) return;
    auto imports=reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base+rva);
    for (;imports->Name;++imports) {
        if (_stricmp(reinterpret_cast<char*>(base+imports->Name),"d3dx9_43.dll") || !imports->OriginalFirstThunk) continue;
        auto names=reinterpret_cast<IMAGE_THUNK_DATA32*>(base+imports->OriginalFirstThunk);
        auto addresses=reinterpret_cast<IMAGE_THUNK_DATA32*>(base+imports->FirstThunk);
        for (;names->u1.AddressOfData;++names,++addresses) {
            if (IMAGE_SNAP_BY_ORDINAL32(names->u1.Ordinal)) continue;
            auto entry=reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base+names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<char*>(entry->Name),"D3DXMatrixMultiply")) continue;
            DWORD old=0;
            if (!VirtualProtect(&addresses->u1.Function,sizeof(DWORD),PAGE_READWRITE,&old)) {log("IAT observer patch failed");return;}
            originalMultiply=reinterpret_cast<MultiplyFn>(addresses->u1.Function);
            InterlockedExchange(reinterpret_cast<volatile LONG*>(&addresses->u1.Function),
                static_cast<LONG>(reinterpret_cast<uintptr_t>(&hookedMultiply)));
            DWORD ignored=0; VirtualProtect(&addresses->u1.Function,sizeof(DWORD),old,&ignored);
            log("D3DXMatrixMultiply IAT observer installed; original=%p",originalMultiply);
            return;
        }
    }
    log("D3DXMatrixMultiply import observer unavailable");
}
void discoverRtti() {
    if (!known) return;
    auto base=reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto nt=reinterpret_cast<IMAGE_NT_HEADERS32*>(base+reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    auto sections=IMAGE_FIRST_SECTION(nt);
    auto imageEnd=reinterpret_cast<uintptr_t>(base)+nt->OptionalHeader.SizeOfImage;
    uintptr_t rdata=0,end=0,code=0,codeEnd=0;
    for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i) {
        if (!std::memcmp(sections[i].Name,".rdata",6)) { rdata=reinterpret_cast<uintptr_t>(base)+sections[i].VirtualAddress;end=rdata+sections[i].Misc.VirtualSize; }
        if (!std::memcmp(sections[i].Name,".text",5)) {code=reinterpret_cast<uintptr_t>(base)+sections[i].VirtualAddress;codeEnd=code+sections[i].Misc.VirtualSize;}
    }
    if (!rdata || !code) return;
    for(auto& target:targets) {
        uintptr_t type=reinterpret_cast<uintptr_t>(base)+target.nameRva-8;
        for(uintptr_t p=rdata+12;p+8<end;p+=4) {
            if (*reinterpret_cast<uint32_t*>(p)!=type) continue;
            auto col=p-12;
            auto fields=reinterpret_cast<uint32_t*>(col);
            if (fields[0]!=0 || fields[1]!=0 || fields[4]<rdata || fields[4]>=imageEnd) continue;
            for(uintptr_t q=rdata;q+16<end;q+=4) {
                if (*reinterpret_cast<uint32_t*>(q)!=col) continue;
                auto vt=q+4;
                auto entries=reinterpret_cast<uint32_t*>(vt);
                if (entries[0]<code || entries[0]>=codeEnd || entries[1]<code || entries[1]>=codeEnd) continue;
                if (std::find(target.vtables.begin(),target.vtables.end(),vt)==target.vtables.end()) {
                    target.vtables.push_back(vt);
                    log("RTTI %s: vtable RVA=0x%08x, COL RVA=0x%08x",target.name,
                        static_cast<unsigned>(vt-reinterpret_cast<uintptr_t>(base)),static_cast<unsigned>(col-reinterpret_cast<uintptr_t>(base)));
                }
            }
        }
    }
}
}

void log(const char* format,...) {
    std::lock_guard<std::mutex> lock(logMutex);
    if (!logFile) return;
    fprintf(logFile,"[%lu tid=%lu] ",GetTickCount(),GetCurrentThreadId());
    va_list args; va_start(args,format); vfprintf(logFile,format,args); va_end(args);
    fputc('\n',logFile);
    // Shader/resource creation can burst after a device reset. Do not force a
    // disk write for every draw-path diagnostic while holding the device mutex.
    static DWORD lastFlush=0;
    DWORD now=GetTickCount();
    if(now-lastFlush>=500) {fflush(logFile);lastFlush=now;}
}
const Config& config() { return settings; }
bool knownExecutable() { return known || settings.allowUnknown; }
void initializeHook() {
    std::call_once(initialization,[] {
        wchar_t executable[32768]{}; GetModuleFileNameW(nullptr,executable,32768);
        std::wstring directory(executable); directory.resize(directory.find_last_of(L"\\/")+1);
        auto ini=directory+L"acah_vr.ini";
        shaderDumpDirectory=directory+L"vr-poc\\diagnostics\\live-shaders\\";
        CreateDirectoryW((directory+L"vr-poc\\diagnostics").c_str(),nullptr);
        CreateDirectoryW(shaderDumpDirectory.c_str(),nullptr);
        logFile=_wfsopen((directory+L"acah_vr_hook.log").c_str(),L"w",_SH_DENYNO);
        auto integer=[&](const wchar_t* name,int def) {return static_cast<int>(GetPrivateProfileIntW(L"vr",name,def,ini.c_str()));};
        auto number=[&](const wchar_t* name,float def) {wchar_t buffer[128]{}, fallback[128]{};
            swprintf_s(fallback,L"%.3f",def);GetPrivateProfileStringW(L"vr",name,fallback,buffer,128,ini.c_str());
            float value=static_cast<float>(_wtof(buffer));return std::isfinite(value)?value:def;};
        settings.enabled=integer(L"enabled",1)!=0; settings.transposeWvp=integer(L"transpose_wvp",0)!=0;
        settings.allowUnknown=integer(L"allow_unknown_exe",0)!=0;
        settings.hudEnabled=integer(L"spatial_hud",1)!=0;
        settings.hudDepth=std::clamp(number(L"hud_depth_meters",0),0.0f,1000.0f);
        settings.headPanelLeft=std::clamp(number(L"head_panel_left",0.72f),0.0f,1.0f);
        settings.headPanelTop=std::clamp(number(L"head_panel_top",0.72f),0.0f,1.0f);
        settings.deferredCapture=integer(L"deferred_capture",1)!=0;
        settings.headAwareCulling=integer(L"head_aware_culling",1)!=0;
        settings.units=std::clamp(number(L"units_per_meter",1),0.001f,1000.0f);
        settings.fallbackFov=number(L"fallback_horizontal_fov_degrees",0);
        settings.maxPairs=std::clamp(integer(L"max_clone_pairs",96),8,512);
        settings.maxMegabytes=std::clamp(integer(L"max_clone_megabytes",256),16,1024);
        auto hash=executableHash(); known=hash=="b39bb7b55ed34ed7acc80e46181d1dbf76ad2f13d39b6d401daf4b2a67d6cf9f";
        log("ACAH VR POC v2; hash=%s known=%d enabled=%d units=%.3f spatialHUD=%d HUDdepth=%.3fm panel=%.2f,%.2f deferredCapture=%d",
            hash.c_str(),known,settings.enabled,settings.units,settings.hudEnabled,settings.hudDepth,
            settings.headPanelLeft,settings.headPanelTop,settings.deferredCapture);
        discoverRtti(); if (settings.enabled && knownExecutable()) patchMultiplyImport();
        if(settings.enabled && known)initializeVisibility();
    });
}
void setGameAspect(float value) { if (value>0) aspect.store(value); }
float gameAspect() {return aspect.load();}
void observeProjection(const Matrix& input,const char* source) {
    Matrix m=input;
    if (!perspective(m)) {m=transpose(input);if (!perspective(m)) return;}
    float candidateAspect=m.m[5]/m.m[0];
    if (std::abs(candidateAspect-aspect.load())>0.12f) return; // avoid square shadow maps
    std::lock_guard<std::mutex> lock(projectionMutex);
    if (!observed.serial || std::memcmp(&observed.projection,&m,sizeof(m))) {
        if (!observed.serial || observed.serial<8)
            log("projection observed from %s: x=%.5f y=%.5f z=%.6f w=%.5f depth=%.5f",source,m.m[0],m.m[5],m.m[10],m.m[11],m.m[14]);
        observed.projection=m; ++observed.serial;
    }
}
ProjectionObservation projectionObservation() { std::lock_guard<std::mutex> lock(projectionMutex);return observed; }
void dumpRuntimeObjects() {
    if (!known) {log("RTTI object scan disabled for unknown executable");return;}
    log("RTTI scan beginning; candidates only, no virtual functions are invoked");
    size_t budget=64*1024*1024;unsigned found=0;
    for(uintptr_t address=0x10000;address<0x7fff0000 && budget && found<40;) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void*>(address),&info,sizeof(info))) break;
        uintptr_t next=reinterpret_cast<uintptr_t>(info.BaseAddress)+info.RegionSize;
        DWORD protection=info.Protect&0xff;
        bool writable=protection==PAGE_READWRITE || protection==PAGE_WRITECOPY || protection==PAGE_EXECUTE_READWRITE;
        if(info.State==MEM_COMMIT && writable && !(info.Protect&PAGE_GUARD) && info.RegionSize<=16*1024*1024) {
            size_t bytes=std::min(budget,static_cast<size_t>(info.RegionSize));budget-=bytes;
            auto start=(reinterpret_cast<uintptr_t>(info.BaseAddress)+3)&~uintptr_t(3);
            std::array<uint32_t,16384> copy{};
            for(size_t offset=0;offset<bytes && found<40;offset+=sizeof(copy)) {
                SIZE_T copied=0;
                ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(start+offset),copy.data(),std::min(sizeof(copy),bytes-offset),&copied);
                for(size_t i=0;i<copied/4 && found<40;++i) {
                    uintptr_t p=start+offset+i*4;auto value=copy[i];
                    for(auto& target:targets) if(std::find(target.vtables.begin(),target.vtables.end(),value)!=target.vtables.end()) {
                        if(readable(p,32)) {log("RTTI candidate %s object=%p vtable=%p",target.name,reinterpret_cast<void*>(p),reinterpret_cast<void*>(value));++found;}
                    }
                }
            }
        }
        if(next<=address) break; address=next;
    }
    log("RTTI scan ended: %u candidates",found);
}

ShaderInfo inspectShader(IDirect3DVertexShader9* shader) {
    ShaderInfo result; UINT bytes=0;
    if(FAILED(shader->GetFunction(nullptr,&bytes)) || bytes<4 || bytes>1024*1024) return result;
    std::vector<uint8_t> code(bytes);
    if(FAILED(shader->GetFunction(code.data(),&bytes)))return result;
    result.bytes=bytes;result.hash=2166136261u;
    for(auto b:code)result.hash=(result.hash^b)*16777619u;
    // Metadata is independent of the shader COM object and survives Reset.
    // The game recreates hundreds of byte-identical shaders on each reset.
    static std::mutex cacheMutex;
    static std::unordered_map<uint64_t,ShaderInfo> cache;
    uint64_t cacheKey=(uint64_t(result.hash)<<32)|bytes;
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        auto found=cache.find(cacheKey);if(found!=cache.end())return found->second;
    }
    wchar_t shaderName[32]{};swprintf_s(shaderName,L"%08x.bin",result.hash);
    auto dumpPath=shaderDumpDirectory+shaderName;
    if(GetFileAttributesW(dumpPath.c_str())==INVALID_FILE_ATTRIBUTES) {
        std::ofstream dump(dumpPath,std::ios::binary);dump.write(reinterpret_cast<const char*>(code.data()),code.size());
    }
    auto word=[&](size_t p) {uint32_t v;std::memcpy(&v,code.data()+p,4);return v;};
    auto half=[&](size_t p) {uint16_t v;std::memcpy(&v,code.data()+p,2);return v;};
    for(size_t token=4;token+4<=bytes;) {
        uint32_t instruction=word(token),opcode=instruction&0xffff;
        if(opcode==0xffff)break;
        uint32_t count=opcode==0xfffe ? ((instruction>>16)&0x7fff) : ((instruction>>24)&15);
        size_t next=token+4+size_t(count)*4;
        if(next>bytes || next<=token)break;
        if(opcode==D3DSIO_DCL && count==2) {
            DWORD declaration=word(token+8);
            DWORD type=((declaration&D3DSP_REGTYPE_MASK)>>D3DSP_REGTYPE_SHIFT)|((declaration&D3DSP_REGTYPE_MASK2)>>D3DSP_REGTYPE_SHIFT2);
            UINT index=declaration&D3DSP_REGNUM_MASK;
            if(type==D3DSPR_SAMPLER)result.vertexSamplers|=index<4?(1u<<(16+index)):0xf0000u;
        }
        if(opcode==0xfffe && count>=8 && word(token+4)==0x42415443) {
            size_t base=token+8;
            uint32_t constants=word(base+12),info=word(base+16);
            if(word(base)==28 && constants<=512 && base+info+size_t(constants)*20<=next) {
                for(uint32_t i=0;i<constants;++i) {
                    size_t c=base+info+i*20;uint32_t nameOffset=word(c);
                    if(nameOffset>=next-base || half(c+4)!=2)continue;
                    const char* name=reinterpret_cast<const char*>(code.data()+base+nameOffset);
                    size_t max=next-base-nameOffset;
                    if(!std::memchr(name,0,max))continue;
                    int index=half(c+6),registerCount=half(c+8);
                    if(index+registerCount>256)continue;
                    if(!std::strcmp(name,"ACE_mWVP") && registerCount==4)result.wvp=index;
                    if((!std::strcmp(name,"WorldViewProjection") || !std::strcmp(name,"ForceWorldViewProjection")) && registerCount==4) {
                        result.wvp=index;result.effect=true;
                    }
                    if(!std::strcmp(name,"ACE_mView") && registerCount==4)result.view=index;
                    if(!std::strcmp(name,"ViewMatrix") && registerCount==4)result.view=index;
                    if(!std::strcmp(name,"ACE_mLocal") && registerCount==4)result.local=index;
                    if(!std::strcmp(name,"ACE_vCamPos") && registerCount==1)result.camera=index;
                    if(!std::strcmp(name,"CameraPosition_NONE") && registerCount==1)result.camera=index;
                    if(!std::strcmp(name,"GlobalCameraPosition_NONE") && registerCount==1)result.globalCamera=index;
                    if(!std::strcmp(name,"InverseProjection") && registerCount==4)result.inverseProjection=index;
                    if((!std::strcmp(name,"InverseViewMatrix") || !std::strcmp(name,"ForceInverseViewMatrix")) && registerCount>=2 && registerCount<=3) {
                        result.inverseView=index;result.inverseViewCount=registerCount;
                    }
                    if((!std::strcmp(name,"transform") || !std::strcmp(name,"TRANSFORM")) && registerCount==4)result.hudTransform=index;
                }
            }
        }
        token=next;
    }
    log("shader hash=%08x bytes=%u WVP=c%d View=c%d Local=c%d Camera=c%d UI=c%d effect=%d billboard=c%d:%d",
        result.hash,result.bytes,result.wvp,result.view,result.local,result.camera,result.hudTransform,result.effect,result.inverseView,result.inverseViewCount);
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        cache.emplace(cacheKey,result);
    }
    return result;
}
}
