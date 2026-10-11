#include "stereo.h"
#include <atomic>
#include <new>

namespace {
HMODULE systemD3d9=nullptr;
std::once_flag systemInitialization;
void loadSystem() {
    std::call_once(systemInitialization,[] {
        // A bare name can resolve to this already-loaded proxy even with a
        // system-directory search flag. Use the absolute system DLL path.
        wchar_t systemPath[MAX_PATH]{};
        UINT length=GetSystemDirectoryW(systemPath,MAX_PATH);
        if(length && length<MAX_PATH-10) {
            wcscat_s(systemPath,L"\\d3d9.dll");
            systemD3d9=LoadLibraryExW(systemPath,nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        }
        acah::initializeHook();
        acah::log("system d3d9=%p",systemD3d9);
    });
}
template<class T> T symbol(const char* name) {loadSystem();return reinterpret_cast<T>(GetProcAddress(systemD3d9,name));}
class Device;
class Factory final: public IDirect3D9 {
    std::atomic<ULONG> references_{1};
    IDirect3D9* real_;
public:
    explicit Factory(IDirect3D9* real):real_(real) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** output) override {
        if(!output)return E_POINTER;*output=nullptr;
        if(iid==__uuidof(IUnknown) || iid==__uuidof(IDirect3D9)) {*output=this;AddRef();return S_OK;}
        return real_->QueryInterface(iid,output);
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return ++references_;}
    ULONG STDMETHODCALLTYPE Release() override {ULONG n=--references_;if(!n){real_->Release();delete this;}return n;}
    HRESULT STDMETHODCALLTYPE CreateDevice(UINT,D3DDEVTYPE,HWND,DWORD,D3DPRESENT_PARAMETERS*,IDirect3DDevice9**) override;
#include "factory_forward.inc"
};

class Device final:public IDirect3DDevice9 {
    std::atomic<ULONG> references_{1};
    // D3DCREATE_MULTITHREADED protects individual real-device calls, not the
    // capture/apply/draw/restore transaction or our shared frame bookkeeping.
    std::recursive_mutex deviceMutex_;
    IDirect3DDevice9* real_;
    IDirect3D9* factory_;
    acah::Stereo stereo_;
    HRESULT cooperativeStatus_=S_OK;
    template<class Operation> void instrument(const char* label,Operation&& operation) noexcept {
        try {operation();}
        catch(const std::exception& e) {acah::log("%s exception: %s; rejecting this stereo frame",label,e.what());stereo_.rejectFrame();}
        catch(...) {acah::log("%s exception; rejecting this stereo frame",label);stereo_.rejectFrame();}
    }
    template<class Operation> void mirror(Operation&& operation,bool userMemory=false,UINT primitiveCount=0) {
        instrument("eye draw",[&]{stereo_.draw(operation,userMemory,primitiveCount);});
    }
public:
    Device(IDirect3DDevice9* real,IDirect3D9* factory):real_(real),factory_(factory),stereo_(real) {factory_->AddRef();}
    acah::Stereo& stereo() {return stereo_;}
    std::recursive_mutex& mutex() {return deviceMutex_;}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** output) override {
        if(!output)return E_POINTER;*output=nullptr;
        if(iid==__uuidof(IUnknown) || iid==__uuidof(IDirect3DDevice9)) {*output=this;AddRef();return S_OK;}
        return real_->QueryInterface(iid,output);
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return ++references_;}
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG n=--references_;
        if(!n){stereo_.reset();factory_->Release();real_->Release();delete this;}return n;
    }
    HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D9** output) override {
        if(!output)return D3DERR_INVALIDCALL;*output=factory_;factory_->AddRef();return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override {
        std::lock_guard<std::recursive_mutex> lock(deviceMutex_);
        HRESULT hr=real_->TestCooperativeLevel();
        if(hr!=cooperativeStatus_) {acah::log("native cooperative status %08x -> %08x",cooperativeStatus_,hr);cooperativeStatus_=hr;}
        return hr;
    }
    HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS* parameters) override {
        std::lock_guard<std::recursive_mutex> lock(deviceMutex_);
        DWORD foregroundPid=0;GetWindowThreadProcessId(GetForegroundWindow(),&foregroundPid);
        DWORD start=GetTickCount();
        MEMORYSTATUSEX memory{};memory.dwLength=sizeof(memory);GlobalMemoryStatusEx(&memory);
        void* stack[6]{};USHORT count=CaptureStackBackTrace(0,6,stack,nullptr);
        acah::log("Reset begin windowed=%d size=%ux%u foregroundPid=%lu gamePid=%lu cooperative=%08x availableVirtual=%.1fMiB textureEstimate=%.1fMiB interval=%08x",
            parameters?parameters->Windowed:-1,parameters?parameters->BackBufferWidth:0,parameters?parameters->BackBufferHeight:0,foregroundPid,GetCurrentProcessId(),
            real_->TestCooperativeLevel(),double(memory.ullAvailVirtual)/(1024*1024),double(real_->GetAvailableTextureMem())/(1024*1024),parameters?parameters->PresentationInterval:0);
        acah::log("Reset caller stack count=%u: %p %p %p %p %p %p",count,stack[0],stack[1],stack[2],stack[3],stack[4],stack[5]);
        stereo_.reset();DWORD released=GetTickCount();HRESULT hr=real_->Reset(parameters);
        acah::log("Reset hr=%08x release=%lums driver=%lums total=%lums",hr,released-start,GetTickCount()-released,GetTickCount()-start);return hr;
    }
    HRESULT STDMETHODCALLTYPE Present(const RECT* a,const RECT* b,HWND window,const RGNDATA* region) override {
        std::lock_guard<std::recursive_mutex> lock(deviceMutex_);
        instrument("capture",[&]{stereo_.present();});
        DWORD start=GetTickCount();HRESULT hr=real_->Present(a,b,window,region);
        if(GetTickCount()-start>100)acah::log("slow original Present elapsed=%lums hr=%08x",GetTickCount()-start,hr);
        stereo_.presented(hr);return hr;
    }
    HRESULT STDMETHODCALLTYPE BeginScene() override {std::lock_guard<std::recursive_mutex> lock(deviceMutex_);HRESULT hr=real_->BeginScene();if(SUCCEEDED(hr))instrument("frame start",[&]{stereo_.begin();});return hr;}
    HRESULT STDMETHODCALLTYPE BeginStateBlock() override {std::lock_guard<std::recursive_mutex> lock(deviceMutex_);HRESULT hr=real_->BeginStateBlock();if(SUCCEEDED(hr))stereo_.recording(true);return hr;}
    HRESULT STDMETHODCALLTYPE EndStateBlock(IDirect3DStateBlock9** output) override {
        std::lock_guard<std::recursive_mutex> lock(deviceMutex_);
        HRESULT hr=real_->EndStateBlock(output);if(SUCCEEDED(hr))stereo_.recording(false);return hr;
    }
    HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE type,const D3DMATRIX* matrix) override {
        std::lock_guard<std::recursive_mutex> lock(deviceMutex_);
        HRESULT hr=real_->SetTransform(type,matrix);
        if(SUCCEEDED(hr) && type==D3DTS_PROJECTION && matrix) instrument("projection observation",[&]{
            acah::Matrix copy;std::memcpy(&copy,matrix,sizeof(copy));acah::observeProjection(copy,"SetTransform");});
        return hr;
    }
    HRESULT STDMETHODCALLTYPE SetRenderTarget(DWORD index,IDirect3DSurface9* target) override {
        std::lock_guard<std::recursive_mutex> lock(deviceMutex_);
        HRESULT hr=real_->SetRenderTarget(index,target);if(SUCCEEDED(hr))instrument("render target",[&]{stereo_.renderTarget(index,target);});return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateVertexShader(const DWORD* function,IDirect3DVertexShader9** output) override {
        std::lock_guard<std::recursive_mutex> lock(deviceMutex_);
        HRESULT hr=real_->CreateVertexShader(function,output);if(SUCCEEDED(hr) && output && *output)instrument("shader inspection",[&]{stereo_.shader(*output);});return hr;
    }
    HRESULT STDMETHODCALLTYPE Clear(DWORD count,const D3DRECT* rects,DWORD flags,D3DCOLOR color,float z,DWORD stencil) override {
        std::lock_guard<std::recursive_mutex> lock(deviceMutex_);
        instrument("eye clear",[&]{stereo_.clear(count,rects,flags,color,z,stencil);});return real_->Clear(count,rects,flags,color,z,stencil);
    }
    HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE type,UINT start,UINT count) override {
        std::lock_guard<std::recursive_mutex> lock(deviceMutex_);
        auto operation=[&]{return real_->DrawPrimitive(type,start,count);};mirror(operation,false,count);return operation();
    }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE type,INT base,UINT min,UINT vertices,UINT start,UINT count) override {
        std::lock_guard<std::recursive_mutex> lock(deviceMutex_);
        auto operation=[&]{return real_->DrawIndexedPrimitive(type,base,min,vertices,start,count);};mirror(operation,false,count);return operation();
    }
    HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE type,UINT count,const void* vertices,UINT stride) override {
        std::lock_guard<std::recursive_mutex> lock(deviceMutex_);
        auto operation=[&]{return real_->DrawPrimitiveUP(type,count,vertices,stride);};mirror(operation,true,count);return operation();
    }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE type,UINT min,UINT vertices,UINT count,const void* indices,D3DFORMAT format,const void* data,UINT stride) override {
        std::lock_guard<std::recursive_mutex> lock(deviceMutex_);
        auto operation=[&]{return real_->DrawIndexedPrimitiveUP(type,min,vertices,count,indices,format,data,stride);};mirror(operation,true,count);return operation();
    }
    HRESULT STDMETHODCALLTYPE StretchRect(IDirect3DSurface9* src,const RECT* a,IDirect3DSurface9* dst,const RECT* b,D3DTEXTUREFILTERTYPE filter) override {
        std::lock_guard<std::recursive_mutex> lock(deviceMutex_);
        instrument("eye copy",[&]{stereo_.stretch(src,a,dst,b,filter);});return real_->StretchRect(src,a,dst,b,filter);
    }
    HRESULT STDMETHODCALLTYPE ColorFill(IDirect3DSurface9* target,const RECT* rect,D3DCOLOR color) override {
        std::lock_guard<std::recursive_mutex> lock(deviceMutex_);
        instrument("eye fill",[&]{stereo_.fill(target,rect,color);});return real_->ColorFill(target,rect,color);
    }
    HRESULT STDMETHODCALLTYPE UpdateTexture(IDirect3DBaseTexture9* src,IDirect3DBaseTexture9* dst) override {
        std::lock_guard<std::recursive_mutex> lock(deviceMutex_);
        HRESULT hr=real_->UpdateTexture(src,dst);if(SUCCEEDED(hr))instrument("texture update",[&]{stereo_.updateTexture(src,dst);});return hr;
    }
    HRESULT STDMETHODCALLTYPE UpdateSurface(IDirect3DSurface9* src,const RECT* a,IDirect3DSurface9* dst,const POINT* b) override {
        std::lock_guard<std::recursive_mutex> lock(deviceMutex_);
        HRESULT hr=real_->UpdateSurface(src,a,dst,b);if(SUCCEEDED(hr))instrument("surface update",[&]{stereo_.updateSurface(src,a,dst,b);});return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateQuery(D3DQUERYTYPE,IDirect3DQuery9**) override;
#include "device_forward.inc"
};

class Query final:public IDirect3DQuery9 {
    std::atomic<ULONG> references_{1};IDirect3DQuery9* real_;Device* owner_;bool active_=false;
public:
    Query(IDirect3DQuery9* real,Device* owner):real_(real),owner_(owner) {owner_->AddRef();}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** output) override {
        if(!output)return E_POINTER;*output=nullptr;
        if(iid==__uuidof(IUnknown) || iid==__uuidof(IDirect3DQuery9)){*output=this;AddRef();return S_OK;}
        return real_->QueryInterface(iid,output);
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return ++references_;}
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG n=--references_;if(!n){
            {std::lock_guard<std::recursive_mutex> lock(owner_->mutex());if(active_)owner_->stereo().query(false);real_->Release();}
            owner_->Release();delete this;
        }return n;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** output) override {if(!output)return E_POINTER;*output=owner_;owner_->AddRef();return S_OK;}
    D3DQUERYTYPE STDMETHODCALLTYPE GetType() override {return real_->GetType();}
    DWORD STDMETHODCALLTYPE GetDataSize() override {return real_->GetDataSize();}
    HRESULT STDMETHODCALLTYPE Issue(DWORD flags) override {
        std::lock_guard<std::recursive_mutex> lock(owner_->mutex());
        HRESULT hr=real_->Issue(flags);
        if(SUCCEEDED(hr) && real_->GetType()==D3DQUERYTYPE_OCCLUSION) {
            if((flags&D3DISSUE_BEGIN) && !active_){owner_->stereo().query(true);active_=true;}
            if((flags&D3DISSUE_END) && active_){owner_->stereo().query(false);active_=false;}
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetData(void* output,DWORD size,DWORD flags) override {
        DWORD start=GetTickCount();
        std::lock_guard<std::recursive_mutex> lock(owner_->mutex());
        DWORD entered=GetTickCount();
        HRESULT hr=real_->GetData(output,size,flags);
        if(GetTickCount()-start>100)acah::log("slow native query GetData type=%u flags=%u mutex=%lums driver=%lums hr=%08x",
            real_->GetType(),flags,entered-start,GetTickCount()-entered,hr);
        // Keep readiness/error semantics. A zero-sample query made from the
        // native forward camera cannot prove occlusion in either tracked eye.
        // Actual eye depth testing still hides genuinely occluded geometry.
        if(hr==S_OK && output && size==sizeof(DWORD) && real_->GetType()==D3DQUERYTYPE_OCCLUSION &&
            !*static_cast<DWORD*>(output) && owner_->stereo().conservativeVisibility()) {
            *static_cast<DWORD*>(output)=1;
            static std::atomic<uint32_t> zeros{0},report{0};++zeros;
            uint32_t now=GetTickCount(),previous=report.load();
            if(now-previous>3000 && report.compare_exchange_strong(previous,now))acah::log("native zero-sample occlusion retained for eye visibility count=%u",zeros.load());
        }
        return hr;
    }
};
HRESULT Device::CreateQuery(D3DQUERYTYPE type,IDirect3DQuery9** output) {
    std::lock_guard<std::recursive_mutex> lock(deviceMutex_);
    HRESULT hr=real_->CreateQuery(type,output);
    if(SUCCEEDED(hr) && output && *output){auto real=*output;auto wrapped=new(std::nothrow) Query(real,this);if(wrapped)*output=wrapped;}
    return hr;
}
HRESULT Factory::CreateDevice(UINT adapter,D3DDEVTYPE type,HWND window,DWORD flags,D3DPRESENT_PARAMETERS* parameters,IDirect3DDevice9** output) {
    HRESULT hr=real_->CreateDevice(adapter,type,window,flags,parameters,output);
    acah::log("CreateDevice adapter=%u flags=%08x hr=%08x",adapter,flags,hr);
    if(SUCCEEDED(hr) && output && *output && acah::config().enabled) {
        IDirect3DDevice9* real=*output;
        try {*output=new Device(real,this);}catch(const std::exception& e){acah::log("device wrapper unavailable: %s",e.what());*output=real;}
    }
    return hr;
}
}

extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT version) {
    auto create=symbol<IDirect3D9* (WINAPI*)(UINT)>("Direct3DCreate9");if(!create)return nullptr;
    auto real=create(version);if(!real)return nullptr;
    auto wrapped=new(std::nothrow) Factory(real);return wrapped?wrapped:real;
}
extern "C" HRESULT WINAPI Direct3DCreate9Ex(UINT version,IDirect3D9Ex** output) {
    auto create=symbol<HRESULT (WINAPI*)(UINT,IDirect3D9Ex**)>("Direct3DCreate9Ex");
    acah::log("Direct3DCreate9Ex requested; forwarded without instrumentation");return create?create(version,output):D3DERR_NOTAVAILABLE;
}
extern "C" int WINAPI D3DPERF_BeginEvent(DWORD color,LPCWSTR text) {auto fn=symbol<int (WINAPI*)(DWORD,LPCWSTR)>("D3DPERF_BeginEvent");return fn?fn(color,text):-1;}
extern "C" int WINAPI D3DPERF_EndEvent() {auto fn=symbol<int (WINAPI*)()>("D3DPERF_EndEvent");return fn?fn():-1;}
extern "C" void WINAPI D3DPERF_SetMarker(DWORD color,LPCWSTR text) {auto fn=symbol<void (WINAPI*)(DWORD,LPCWSTR)>("D3DPERF_SetMarker");if(fn)fn(color,text);}
extern "C" void WINAPI D3DPERF_SetRegion(DWORD color,LPCWSTR text) {auto fn=symbol<void (WINAPI*)(DWORD,LPCWSTR)>("D3DPERF_SetRegion");if(fn)fn(color,text);}
extern "C" BOOL WINAPI D3DPERF_QueryRepeatFrame() {auto fn=symbol<BOOL (WINAPI*)()>("D3DPERF_QueryRepeatFrame");return fn?fn():FALSE;}
extern "C" void WINAPI D3DPERF_SetOptions(DWORD options) {auto fn=symbol<void (WINAPI*)(DWORD)>("D3DPERF_SetOptions");if(fn)fn(options);}
extern "C" DWORD WINAPI D3DPERF_GetStatus() {auto fn=symbol<DWORD (WINAPI*)()>("D3DPERF_GetStatus");return fn?fn():0;}
BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) {return TRUE;}
