#include "stereo.h"
#include "hud.h"
#include "tree.h"
#include <algorithm>
#include <cmath>

namespace acah {
static_assert(kTreeConstants==kHudConstants && kTreeConstantCount==kHudConstantCount);
namespace {
std::array<Ptr<IDirect3DSurface9>,4> renderTargets(IDirect3DDevice9* device) {
    std::array<Ptr<IDirect3DSurface9>,4> targets;
    for(unsigned i=0;i<targets.size();++i)device->GetRenderTarget(i,targets[i].GetAddressOf());
    return targets;
}
uint64_t bytesFor(const D3DSURFACE_DESC& d) {
    uint32_t bytes=4;
    if(d.Format==D3DFMT_A16B16G16R16F || d.Format==D3DFMT_A16B16G16R16)bytes=8;
    if(d.Format==D3DFMT_A32B32G32R32F)bytes=16;
    if(d.Format==D3DFMT_R16F || d.Format==D3DFMT_D16)bytes=2;
    uint32_t samples=d.MultiSampleType==D3DMULTISAMPLE_NONE?1:std::max(2u,static_cast<uint32_t>(d.MultiSampleType));
    return uint64_t(d.Width)*d.Height*bytes*samples*2;
}
DWORD samplerIndex(unsigned index) {return index<16?index:D3DVERTEXTEXTURESAMPLER0+index-16;}
bool sceneViewport(const D3DVIEWPORT9& viewport,uint32_t width,uint32_t height) {
    return viewport.Width>=160 && viewport.Height>=90 && height &&
        std::abs(float(viewport.Width)/viewport.Height-float(width)/height)<0.03f;
}
bool affineView(const Matrix& view) {
    return finite(view) && std::abs(view.m[3])<1e-4f && std::abs(view.m[7])<1e-4f &&
        std::abs(view.m[11])<1e-4f && std::abs(view.m[15]-1)<1e-4f;
}
struct Elapsed {
    double& total;LARGE_INTEGER start{};
    explicit Elapsed(double& value):total(value) {QueryPerformanceCounter(&start);}
    ~Elapsed() {LARGE_INTEGER end{},frequency{};QueryPerformanceCounter(&end);QueryPerformanceFrequency(&frequency);
        total+=1000.0*double(end.QuadPart-start.QuadPart)/double(frequency.QuadPart);}
};
}
Stereo::Stereo(IDirect3DDevice9* device):device_(device) {
    if(config().enabled) {
        try {mapping_=gameMapping();
            auto c=mapping_->get();
            LONG pid=InterlockedCompareExchange(&c->gamePid,GetCurrentProcessId(),0);
            if(pid && pid!=static_cast<LONG>(GetCurrentProcessId())) {
                HANDLE process=OpenProcess(SYNCHRONIZE,FALSE,pid);
                bool alive=process && WaitForSingleObject(process,0)==WAIT_TIMEOUT;
                if(process)CloseHandle(process);
                if(alive)throw std::runtime_error("another game owns the IPC mapping");
                if(InterlockedCompareExchange(&c->gamePid,GetCurrentProcessId(),pid)!=pid)
                    throw std::runtime_error("another game acquired the IPC mapping during startup");
                // Recover abandoned writer slots; a host-owned reader slot stays intact.
                for(auto& frame:c->frames)InterlockedCompareExchange(&frame.state,0,1);
            }
        } catch(const std::exception& e) {log("IPC disabled: %s",e.what());mapping_.reset();}
    }
    refreshBackbuffer();
}
Stereo::~Stereo() {
    reset();
    if(mapping_)InterlockedCompareExchange(&mapping_->get()->gamePid,0,GetCurrentProcessId());
}
void Stereo::reset() {
    surfaces_.clear();textures_.clear();shaders_.clear();pixelShaders_.clear();
    sampledHudShaders_.clear();
    backbuffer_.Reset();resolve_.Reset();readback_.Reset();msaaResolve_.Reset();
    for(auto& slot:captures_) {releaseCapture(slot);slot=CaptureSlot{};}
    captureOrder_=publishedOrder_=0;captureDropped_=readbackBusy_=0;deferredUnavailable_=false;sceneViewValid_=false;
    renderSamples_=captureSamples_=queueSamples_=0;captureAgeTotal_=0;
    lastStatus_=GetTickCount();
    cloneBytes_=0;captureWidth_=captureHeight_=width_=height_=0;
    started_=stereo_=projectionValid_=false;activeQueries_=0;
    log("device resources released for reset/shutdown");
}
bool Stereo::refreshBackbuffer() {
    if(backbuffer_)return true;
    if(FAILED(device_->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,backbuffer_.GetAddressOf())))return false;
    D3DSURFACE_DESC d{};backbuffer_->GetDesc(&d);width_=d.Width;height_=d.Height;
    if(!width_ || !height_)return false;
    setGameAspect(float(width_)/height_);
    log("backbuffer %ux%u format=%u MSAA=%u",width_,height_,d.Format,d.MultiSampleType);
    if(width_>kWidth || height_>kHeight)log("native capture unsupported above %ux%u; no automatic downscale",kWidth,kHeight);
    return true;
}
void Stereo::startFrame() {
    if(started_)return;
    started_=true;draws_=transformed_=effectDraws_=hudDraws_=treeDraws_=failures_=querySkipped_=scissorExpanded_=0;replayMilliseconds_=0;
    tracking_={};stereo_=false;projectionValid_=false;
    for(auto& math:eyeMath_)math.present=false;
    eyeMathNext_=0;
    if(mapping_ && hostAlive(mapping_->get()) && readTracking(mapping_->get(),tracking_))
        stereo_=tracking_.valid && tracking_.stereoRequested && knownExecutable();
    if(FAILED(device_->TestCooperativeLevel()))stereo_=false;
    if(stereo_ && !refreshBackbuffer()) {stereo_=false;++failures_;}
    if(stereo_ && (width_>kWidth || height_>kHeight)) {stereo_=false;++failures_;}
    if(stereo_) {
        auto observation=projectionObservation();
        if(observation.serial) {projection_=observation.projection;projectionValid_=true;projectionSerial_=observation.serial;}
        if(!projectionValid_) {
            D3DMATRIX fixed{};
            if(SUCCEEDED(device_->GetTransform(D3DTS_PROJECTION,&fixed))) {
                Matrix candidate;std::memcpy(&candidate,&fixed,sizeof(candidate));
                if(perspective(candidate)) {projection_=candidate;projectionValid_=true;}
            }
        }
        bool recognized=std::any_of(shaders_.begin(),shaders_.end(),[](const auto& shader){return shader.second.info.wvp>=0;});
        // A failed prerequisite must remain a capture-only frame. Do not replay
        // thousands of draws just to discover at Present that stereo is unusable.
        if(!recognized || (!projectionValid_ && config().fallbackFov<=1))stereo_=false;
    }
    if(GetAsyncKeyState(VK_F7)&1)dumpRuntimeObjects();
}
void Stereo::shader(IDirect3DVertexShader9* shader) {
    if(!shader)return;
    Ptr<IDirect3DVertexShader9> held(shader);
    ShaderEntry entry;entry.original=held;entry.info=inspectShader(shader);
    shaders_[shader]=std::move(entry);
}
Ptr<IDirect3DSurface9> Stereo::cloneSurface(IDirect3DSurface9* source,int eye) {
    if(!source)return {};
    auto found=surfaces_.find(source);
    if(found!=surfaces_.end())return found->second.eyes[eye];
    D3DSURFACE_DESC d{};
    if(FAILED(source->GetDesc(&d)))return {};
    if(!(d.Usage&(D3DUSAGE_RENDERTARGET|D3DUSAGE_DEPTHSTENCIL)))return Ptr<IDirect3DSurface9>(source);
    auto fail=[&](const char* reason) {++failures_;if(failures_<4)log("clone unavailable %ux%u format=%u: %s",d.Width,d.Height,d.Format,reason);return Ptr<IDirect3DSurface9>{};};
    if(surfaces_.size()>=config().maxPairs)return fail("pair limit");
    if(cloneBytes_+bytesFor(d)>uint64_t(config().maxMegabytes)*1024*1024)return fail("memory budget");
    SurfacePair pair;pair.original=source;
    Ptr<IDirect3DTexture9> original;
    if(SUCCEEDED(source->GetContainer(__uuidof(IDirect3DTexture9),reinterpret_cast<void**>(original.GetAddressOf())))) {
        auto key=static_cast<IDirect3DBaseTexture9*>(original.Get());
        auto texture=textures_.find(key);
        if(texture==textures_.end()) {
            TexturePair entry;entry.original=original;
            D3DSURFACE_DESC top{};original->GetLevelDesc(0,&top);
            uint64_t textureBytes=0;
            for(UINT mip=0;mip<original->GetLevelCount();++mip) {
                D3DSURFACE_DESC level{};
                if(FAILED(original->GetLevelDesc(mip,&level)))return fail("GetLevelDesc");
                textureBytes+=bytesFor(level);
            }
            if(cloneBytes_+textureBytes>uint64_t(config().maxMegabytes)*1024*1024)return fail("texture memory budget");
            for(int i=0;i<2;++i) {
                HRESULT hr=device_->CreateTexture(top.Width,top.Height,original->GetLevelCount(),top.Usage,top.Format,D3DPOOL_DEFAULT,entry.eyes[i].GetAddressOf(),nullptr);
                if(FAILED(hr))return fail("CreateTexture");
            }
            cloneBytes_+=textureBytes;
            texture=textures_.emplace(key,std::move(entry)).first;
        }
        UINT level=0;bool matched=false;
        for(;level<original->GetLevelCount();++level) {
            Ptr<IDirect3DSurface9> test;original->GetSurfaceLevel(level,test.GetAddressOf());
            if(test.Get()==source) {matched=true;break;}
        }
        if(!matched)return fail("mip identity");
        for(int i=0;i<2;++i)if(FAILED(texture->second.eyes[i]->GetSurfaceLevel(level,pair.eyes[i].GetAddressOf())))return fail("GetSurfaceLevel");
    } else {
        // Cube surfaces are not redirected without matching cube-texture sampling.
        Ptr<IDirect3DCubeTexture9> cube;
        if(SUCCEEDED(source->GetContainer(__uuidof(IDirect3DCubeTexture9),reinterpret_cast<void**>(cube.GetAddressOf()))))return fail("cube RT unsupported");
        for(int i=0;i<2;++i) {
            HRESULT hr=(d.Usage&D3DUSAGE_DEPTHSTENCIL)?
                device_->CreateDepthStencilSurface(d.Width,d.Height,d.Format,d.MultiSampleType,d.MultiSampleQuality,FALSE,pair.eyes[i].GetAddressOf(),nullptr):
                device_->CreateRenderTarget(d.Width,d.Height,d.Format,d.MultiSampleType,d.MultiSampleQuality,FALSE,pair.eyes[i].GetAddressOf(),nullptr);
            if(FAILED(hr))return fail("CreateSurface");
        }
        cloneBytes_+=bytesFor(d);
    }
    if(d.Usage&D3DUSAGE_RENDERTARGET) {
        for(auto& target:pair.eyes) {
            // Seed persistent color buffers once; subsequent writes stay separate.
            HRESULT hr=device_->StretchRect(source,nullptr,target.Get(),nullptr,D3DTEXF_NONE);
            if(FAILED(hr))device_->ColorFill(target.Get(),nullptr,0);
        }
    }
    log("clone pair %p %ux%u format=%u usage=%08x budget=%.1f MiB",source,d.Width,d.Height,d.Format,d.Usage,double(cloneBytes_)/(1024*1024));
    auto inserted=surfaces_.emplace(source,std::move(pair));
    return inserted.first->second.eyes[eye];
}
bool Stereo::saveEyeState(EyeState& state,bool userMemory,uint32_t vertexSamplers,bool textures) {
    state.targets=renderTargets(device_);if(!state.targets[0])return false;
    HRESULT depth=device_->GetDepthStencilSurface(state.depth.GetAddressOf());
    if(FAILED(depth) && depth!=D3DERR_NOTFOUND)return false;
    if(FAILED(device_->GetViewport(&state.viewport)) || FAILED(device_->GetScissorRect(&state.scissor)))return false;
    if(textures) {
        Ptr<IDirect3DPixelShader9> pixel;device_->GetPixelShader(pixel.GetAddressOf());
        auto found=pixelShaders_.find(pixel.Get());
        if(found==pixelShaders_.end()) {
            PixelEntry entry;entry.original=pixel;entry.valid=hudTextureSamplers(pixel.Get(),entry.samplers);
            found=pixelShaders_.emplace(pixel.Get(),std::move(entry)).first;
        }
        state.pixelSamplers=found->second.samplers;state.pixelSamplersValid=found->second.valid;
        uint32_t mask=(state.pixelSamplersValid?state.pixelSamplers:0xffff)|vertexSamplers;
        for(unsigned i=0;i<state.textures.size();++i)if(mask&(1u<<i))
            state.textureValid[i]=SUCCEEDED(device_->GetTexture(samplerIndex(i),state.textures[i].GetAddressOf()));
    }
    state.userMemory=userMemory;
    if(userMemory && (FAILED(device_->GetStreamSource(0,state.stream.GetAddressOf(),&state.offset,&state.stride)) ||
        FAILED(device_->GetIndices(state.indices.GetAddressOf()))))return false;
    return true;
}
bool Stereo::restoreEyeState(EyeState& state) {
    bool ok=true;
    auto check=[&](HRESULT hr,const char* operation) {
        if(FAILED(hr)){ok=false;++failures_;if(failures_<4)log("eye state restore failed: %s hr=%08x",operation,hr);}
    };
    if(state.targetsChanged) {
        // Clones preserve dimensions, format and MSAA. Native/left/right
        // bindings therefore remain compatible during these replacements.
        // Empty MRT slots were never changed and need no clear/rebind calls.
        for(unsigned i=0;i<state.targets.size();++i)if(state.targets[i])
            check(device_->SetRenderTarget(i,state.targets[i].Get()),"render target");
        if(state.depth)check(device_->SetDepthStencilSurface(state.depth.Get()),"depth");
        check(device_->SetViewport(&state.viewport),"viewport");
        check(device_->SetScissorRect(&state.scissor),"scissor");
    }
    for(unsigned i=0;i<state.textures.size();++i)if(state.textureChanged[i])
        check(device_->SetTexture(samplerIndex(i),state.textures[i].Get()),"texture");
    if(state.wvpChanged)check(device_->SetVertexShaderConstantF(state.wvpRegister,state.wvp.m,4),"WVP");
    if(state.sceneChanged)for(unsigned i=0;i<state.sceneConstantCount;++i) {
        const auto& value=state.sceneConstants[i];
        check(device_->SetVertexShaderConstantF(value.index,value.original.m,value.count),"scene view/billboard/camera");
    }
    if(state.hudChanged || state.treeChanged) {
        check(device_->SetVertexShader(state.vertex.Get()),"replay vertex shader");
        check(device_->SetVertexShaderConstantF(kHudConstants,state.hudConstants.data(),kHudConstantCount),"replay constants");
    }
    if(state.hudChanged) {
        check(device_->SetRenderState(D3DRS_ZENABLE,state.zEnabled),"HUD depth test");
        check(device_->SetRenderState(D3DRS_ZWRITEENABLE,state.zWrite),"HUD depth write");
    }
    if(state.userMemory) {
        check(device_->SetStreamSource(0,state.stream.Get(),state.offset,state.stride),"stream 0");
        check(device_->SetIndices(state.indices.Get()),"indices");
    }
    if(!ok)stereo_=false;
    return ok;
}
bool Stereo::bindEye(int eye,EyeState& state) {
    const auto& targets=state.targets;
    auto depth=state.depth.Get();
    if(!targets[0])return false;
    std::array<Ptr<IDirect3DSurface9>,4> clones;
    for(unsigned i=0;i<targets.size();++i) {
        if(!targets[i])continue;
        clones[i]=cloneSurface(targets[i].Get(),eye);if(!clones[i])return false;
    }
    Ptr<IDirect3DSurface9> clonedDepth;
    if(depth) {clonedDepth=cloneSurface(depth,eye);if(!clonedDepth)return false;}
    state.targetsChanged=true;
    for(unsigned i=0;i<clones.size();++i) {
        if(clones[i] && FAILED(device_->SetRenderTarget(i,clones[i].Get())))return false;
    }
    if(clonedDepth && FAILED(device_->SetDepthStencilSurface(clonedDepth.Get())))return false;
    if(FAILED(device_->SetViewport(&state.viewport)) || FAILED(device_->SetScissorRect(&state.scissor)))return false;
    // Only textures that have been used as render targets have an eye counterpart.
    for(unsigned i=0;i<state.textures.size();++i) {
        if(!state.textureValid[i] || !state.textures[i])continue;
        auto entry=textures_.find(state.textures[i].Get());
        if(entry!=textures_.end()) {
            state.textureChanged[i]=true;
            if(FAILED(device_->SetTexture(samplerIndex(i),entry->second.eyes[eye].Get())))return false;
        }
    }
    return true;
}
const Stereo::EyeMath& Stereo::eyeMath(const Matrix& projection) {
    for(const auto& math:eyeMath_)if(math.present && !std::memcmp(&math.native,&projection,sizeof(projection)))return math;
    auto& math=eyeMath_[eyeMathNext_++%eyeMath_.size()];math=EyeMath{};math.present=true;math.native=projection;
    math.valid=invert(projection,math.inverseProjection);math.hudValid=true;
    for(int eye=0;eye<2;++eye) {
        if(!eyeMatrices(tracking_,eye,projection,config().units,math.inverseRelative[eye],math.projection[eye]) ||
            !invert(math.inverseRelative[eye],math.relative[eye]))math.valid=false;
        math.correction[eye]=multiply(multiply(math.inverseProjection,math.inverseRelative[eye]),math.projection[eye]);
        if(!finite(math.correction[eye]))math.valid=false;
        if(!hudCorrection(tracking_,eye,projection,config().units,config().hudDepth,math.hud[eye]))math.hudValid=false;
    }
    return math;
}
void Stereo::draw(const std::function<HRESULT()>& operation,bool userMemory,UINT primitiveCount) {
    startFrame();++draws_;
    if(recording_ || !knownExecutable())return;
    Ptr<IDirect3DVertexShader9> vertex;device_->GetVertexShader(vertex.GetAddressOf());
    ShaderInfo info;
    ShaderEntry* shaderEntry=nullptr;
    if(vertex) {
        auto entry=shaders_.find(vertex.Get());
        if(entry==shaders_.end()) {shader(vertex.Get());entry=shaders_.find(vertex.Get());}
        shaderEntry=&entry->second;info=shaderEntry->info;
    }
    if(info.wvp>=0 && sampledShaders_.size()<24 && sampledShaders_.insert(info.hash).second) {
        Ptr<IDirect3DSurface9> target;device_->GetRenderTarget(0,target.GetAddressOf());D3DSURFACE_DESC desc{};
        if(target)target->GetDesc(&desc);
        log("matrix sample shader=%08x RT=%ux%u WVP=%d View=%d Local=%d",info.hash,desc.Width,desc.Height,info.wvp,info.view,info.local);
        auto dump=[&](const char* name,int reg) {
            if(reg<0)return;Matrix value{};HRESULT hr=device_->GetVertexShaderConstantF(reg,value.m,4);
            log("matrix sample %08x %s hr=%08x: %.9g %.9g %.9g %.9g; %.9g %.9g %.9g %.9g; %.9g %.9g %.9g %.9g; %.9g %.9g %.9g %.9g",info.hash,name,hr,
                value.m[0],value.m[1],value.m[2],value.m[3],value.m[4],value.m[5],value.m[6],value.m[7],
                value.m[8],value.m[9],value.m[10],value.m[11],value.m[12],value.m[13],value.m[14],value.m[15]);
        };dump("WVP",info.wvp);dump("View",info.view);dump("Local",info.local);
    }
    // When a shader exposes all three matrices, recover P = inverse(V) * inverse(W) * WVP.
    // Accept it only if it has a validated perspective shape and the game's aspect.
    if(!info.effect && info.wvp>=0 && info.view>=0 && info.local>=0) {
        Matrix world{},view{},wvp{},inverseWorld{},inverseView{};
        if(SUCCEEDED(device_->GetVertexShaderConstantF(info.local,world.m,4)) &&
            SUCCEEDED(device_->GetVertexShaderConstantF(info.view,view.m,4)) &&
            SUCCEEDED(device_->GetVertexShaderConstantF(info.wvp,wvp.m,4))) {
            if(config().transposeWvp){world=transpose(world);view=transpose(view);wvp=transpose(wvp);}
            if(invert(world,inverseWorld) && invert(view,inverseView))
                observeProjection(multiply(multiply(inverseView,inverseWorld),wvp),"reflected W/View/WVP matrices");
        }
    }
    auto observation=projectionObservation();
    if(observation.serial) {projection_=observation.projection;projectionValid_=true;projectionSerial_=observation.serial;}
    D3DMATRIX fixedProjection{};
    if(!projectionValid_ && SUCCEEDED(device_->GetTransform(D3DTS_PROJECTION,&fixedProjection))) {
        Matrix candidate;std::memcpy(&candidate,&fixedProjection,sizeof(candidate));
        if(perspective(candidate)) {projection_=candidate;projectionValid_=true;}
    }
    if(!projectionValid_ && config().fallbackFov>1 && config().fallbackFov<175) {
        float horizontal=config().fallbackFov*DirectX::XM_PI/180;
        float vertical=2*std::atan(std::tan(horizontal/2)*float(height_)/width_);
        projection_=store(DirectX::XMMatrixPerspectiveFovLH(vertical,float(width_)/height_,0.1f,100000.0f));
        projectionValid_=true;
    }
    if(!stereo_)return;
    if(activeQueries_) {++querySkipped_;return;}
    Elapsed replay(replayMilliseconds_);
    EyeState saved;if(!saveEyeState(saved,userMemory,vertex?info.vertexSamplers:0xf0000)) {++failures_;stereo_=false;return;}
    const auto& targets=saved.targets;
    D3DSURFACE_DESC description{};targets[0]->GetDesc(&description);
    // Scene layers also render below full resolution. Correct matching-aspect
    // viewports while keeping square shadow maps on their original projection.
    bool worldTarget=sceneViewport(saved.viewport,width_,height_);
    bool hudTarget=description.Width==width_ && description.Height==height_;
    Matrix originalWvp{};
    bool modify=worldTarget && projectionValid_ && info.wvp>=0 &&
        SUCCEEDED(device_->GetVertexShaderConstantF(info.wvp,originalWvp.m,4));
    DWORD worldScissorEnabled=0;
    RECT eyeScissor{static_cast<LONG>(saved.viewport.X),static_cast<LONG>(saved.viewport.Y),
        static_cast<LONG>(saved.viewport.X+saved.viewport.Width),static_cast<LONG>(saved.viewport.Y+saved.viewport.Height)};
    bool expandScissor=modify && SUCCEEDED(device_->GetRenderState(D3DRS_SCISSORTESTENABLE,&worldScissorEnabled)) &&
        worldScissorEnabled && std::memcmp(&saved.scissor,&eyeScissor,sizeof(RECT))!=0;
    if(expandScissor)++scissorExpanded_;
    saved.wvpRegister=info.wvp;saved.wvp=originalWvp;
    bool treeSprite=knownTreeShader(info.hash),modifyTree=false;
    Matrix drawProjection=projection_;
    if(modify && treeSprite && shaderEntry) {
        Matrix treeProjection{};
        bool validProjection=SUCCEEDED(device_->GetVertexShaderConstantF(kTreeProjection,treeProjection.m,4));
        if(config().transposeWvp)treeProjection=transpose(treeProjection);
        validProjection=validProjection && perspective(treeProjection);
        if(validProjection)drawProjection=treeProjection;
        if(!shaderEntry->treeAttempted) {
            shaderEntry->treeAttempted=true;D3DCAPS9 caps{};
            if(SUCCEEDED(device_->GetDeviceCaps(&caps)) && caps.MaxVertexShaderConst>=kTreeConstants+kTreeConstantCount)
                shaderEntry->tree=createTreeShader(device_,vertex.Get(),info.hash);
        }
        modifyTree=validProjection && shaderEntry->tree &&
            SUCCEEDED(device_->GetVertexShaderConstantF(kTreeConstants,saved.hudConstants.data(),kTreeConstantCount));
        saved.vertex=vertex;
        // Partial depth-only correction is the original tree drift bug. Reject
        // stereo if this known tree path cannot receive its position patch.
        if(!modifyTree) {++failures_;stereo_=false;log("tree stereo path unavailable shader=%08x projection=%d",info.hash,validProjection);return;}
    }
    auto saveConstant=[&](int index,UINT count) {
        if(index<0)return;
        auto& value=saved.sceneConstants[saved.sceneConstantCount];value.index=index;value.count=count;
        if(SUCCEEDED(device_->GetVertexShaderConstantF(index,value.original.m,count)))++saved.sceneConstantCount;
    };
    if(modify && !treeSprite) {
        saveConstant(info.view,4);saveConstant(info.inverseView,info.inverseViewCount);
        saveConstant(info.camera,1);saveConstant(info.globalCamera,1);saveConstant(info.inverseProjection,4);
        if(!info.effect && info.view>=0)for(unsigned i=0;i<saved.sceneConstantCount;++i) {
            if(saved.sceneConstants[i].index==info.view) {
                Matrix view=config().transposeWvp?transpose(saved.sceneConstants[i].original):saved.sceneConstants[i].original;
                if(affineView(view)) {sceneView_=view;sceneViewValid_=true;}
            }
        }
    }
    bool modifyHud=false;DWORD scissorEnabled=0;
    bool headPanel=false;
    if(config().hudEnabled && hudTarget && projectionValid_ && transformed_>0 &&
        info.wvp<0 && info.hudTransform>=0 && knownHudShader(info.hash)) {
        Matrix transform{};DWORD zFunction=0;
        bool flat=SUCCEEDED(device_->GetVertexShaderConstantF(info.hudTransform,transform.m,4)) && screenSpaceTransform(transform);
        bool depthSaved=SUCCEEDED(device_->GetRenderState(D3DRS_ZENABLE,&saved.zEnabled)) &&
            SUCCEEDED(device_->GetRenderState(D3DRS_ZWRITEENABLE,&saved.zWrite));
        device_->GetRenderState(D3DRS_ZFUNC,&zFunction);
        // Screen copies use these primitive families too. Moving a sampled eye
        // render target would rotate the scene twice; only atlas/primitive UI is eligible.
        bool samplesScene=!saved.pixelSamplersValid;
        // A large textured overlay/target icon is commonly one or two triangles.
        // Restrict the corner exception to batches, rather than splitting a
        // screen-sized quad between head and aircraft coordinates.
        headPanel=saved.pixelSamplers!=0 && primitiveCount>=8;
        for(unsigned i=0;i<16;++i)if(saved.pixelSamplers&(1u<<i)) {
            // Ignore unused, stale sampler bindings left over from the scene.
            if(!saved.textureValid[i] || (saved.textures[i] && textures_.find(saved.textures[i].Get())!=textures_.end()))samplesScene=true;
        }
        if(sampledHudShaders_.insert(info.hash).second)
            log("HUD candidate=%08x RT=%ux%u flat=%d Zenable=%u Zwrite=%u Zfunc=%u samplesScene=%d texturedPanel=%d transform=c%d",
                info.hash,description.Width,description.Height,flat,saved.zEnabled,saved.zWrite,zFunction,samplesScene,headPanel,info.hudTransform);
        if(flat && depthSaved && !samplesScene && shaderEntry) {
            if(!shaderEntry->hudAttempted) {
                shaderEntry->hudAttempted=true;
                D3DCAPS9 caps{};
                if(SUCCEEDED(device_->GetDeviceCaps(&caps)) && caps.MaxVertexShaderConst>=kHudConstants+kHudConstantCount)
                    shaderEntry->hud=createHudShader(device_,vertex.Get(),info.hash);
                if(!shaderEntry->hud)log("spatial HUD shader=%08x unavailable; original UI preserved",info.hash);
            }
            modifyHud=shaderEntry->hud && SUCCEEDED(device_->GetVertexShaderConstantF(kHudConstants,saved.hudConstants.data(),kHudConstantCount)) &&
                SUCCEEDED(device_->GetRenderState(D3DRS_SCISSORTESTENABLE,&scissorEnabled));
            saved.vertex=vertex;
        }
    }
    struct Restore {std::function<void()> action;~Restore(){action();}};
    Restore restore{[&]{restoreEyeState(saved);}};
    bool transformed=false;unsigned hudEyes=0,treeEyes=0;
    const EyeMath* math=(modify || modifyHud)?&eyeMath(drawProjection):nullptr;
    if(math && (!math->valid || (modifyHud && !math->hudValid))) {++failures_;stereo_=false;return;}
    Matrix mathematical{},cameraToLocal{},cameraToWorld{};
    bool localValid=false,worldValid=false;
    if(modify) {
        mathematical=config().transposeWvp && !info.effect?transpose(originalWvp):originalWvp;
        // These native-space inverses are identical for both eyes. Most world
        // shaders do not expose a camera position and need neither inverse.
        if(info.effect && info.camera>=0)
            localValid=invert(multiply(mathematical,math->inverseProjection),cameraToLocal);
        if(sceneViewValid_ && (info.globalCamera>=0 || (!info.effect && info.camera>=0)))
            worldValid=invert(sceneView_,cameraToWorld);
    }
    for(int eye=0;eye<2;++eye) {
        // Derive BOTH eyes from saved native values. Directly switch to the
        // next eye, then restore once before the original desktop operation.
        // UP draws use their supplied data again; native stream/index bindings
        // are restored by the final guard, including all failure exits.
        if(!bindEye(eye,saved)) {++failures_;stereo_=false;break;}
        // A forward-camera pixel bounding box does not bound the reprojected
        // object. The eye viewport is conservative; restoreEyeState restores
        // the native rectangle before the desktop draw (including error exits).
        if(expandScissor && FAILED(device_->SetScissorRect(&eyeScissor))) {++failures_;stereo_=false;break;}
        if(modify) {
            const Matrix& adjustment=math->correction[eye];
            {
                Matrix changed=multiply(mathematical,adjustment);
                if(config().transposeWvp && !info.effect)changed=transpose(changed);
                saved.wvpChanged=true;
                if(SUCCEEDED(device_->SetVertexShaderConstantF(info.wvp,changed.m,4)))transformed=true;
                else ++failures_;
                const Matrix& inverseRelative=math->inverseRelative[eye];
                const Matrix& eyeProjection=math->projection[eye];
                const Matrix& relative=math->relative[eye];
                for(unsigned i=0;i<saved.sceneConstantCount;++i) {
                    const auto& value=saved.sceneConstants[i];Matrix updated=value.original;bool update=false;
                    if(value.index==info.view) {
                        Matrix view=config().transposeWvp && !info.effect?transpose(value.original):value.original;
                        if(affineView(view)) {updated=multiply(view,inverseRelative);update=true;
                            if(config().transposeWvp && !info.effect)updated=transpose(updated);}
                    } else if(value.index==info.inverseView) {
                        Matrix basis=identity();
                        for(unsigned row=0;row<value.count;++row)for(unsigned column=0;column<3;++column)
                            basis.m[row*4+column]=value.original.m[row*4+column];
                        if(value.count==2) {
                            auto right=DirectX::XMVectorSet(basis.m[0],basis.m[1],basis.m[2],0);
                            auto up=DirectX::XMVectorSet(basis.m[4],basis.m[5],basis.m[6],0);
                            auto forward=DirectX::XMVector3Cross(right,up);
                            if(DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(forward))>1e-12f) {
                                forward=DirectX::XMVectorMultiply(DirectX::XMVector3Normalize(forward),DirectX::XMVector3Length(right));
                                DirectX::XMFLOAT3 row;DirectX::XMStoreFloat3(&row,forward);
                                basis.m[8]=row.x;basis.m[9]=row.y;basis.m[10]=row.z;
                            }
                        }
                        Matrix rotation=relative;rotation.m[12]=rotation.m[13]=rotation.m[14]=0;
                        updated=multiply(rotation,basis);
                        for(unsigned row=0;row<value.count;++row)updated.m[row*4+3]=value.original.m[row*4+3];
                        update=true;
                    } else if(value.index==info.inverseProjection) {
                        update=invert(eyeProjection,updated);
                    } else if(value.index==info.camera || value.index==info.globalCamera) {
                        bool local=info.effect && value.index==info.camera;
                        if(local?localValid:worldValid) {
                            auto delta=DirectX::XMVector3TransformNormal(DirectX::XMVectorSet(relative.m[12],relative.m[13],relative.m[14],0),
                                load(local?cameraToLocal:cameraToWorld));
                            DirectX::XMFLOAT3 position;DirectX::XMStoreFloat3(&position,delta);
                            updated.m[0]+=position.x;updated.m[1]+=position.y;updated.m[2]+=position.z;update=true;
                        }
                    }
                    if(update && !finite(updated)) {++failures_;stereo_=false;break;}
                    if(update) {
                        saved.sceneChanged=true;
                        if(FAILED(device_->SetVertexShaderConstantF(value.index,updated.m,value.count))) {++failures_;stereo_=false;break;}
                    }
                }
                if(!stereo_)break;
                if(modifyTree) {
                    Matrix columns=transpose(adjustment);float zero[4]{};saved.treeChanged=true;
                    if(FAILED(device_->SetVertexShader(shaderEntry->tree.Get())) ||
                        FAILED(device_->SetVertexShaderConstantF(kTreeConstants,columns.m,4)) ||
                        FAILED(device_->SetVertexShaderConstantF(kTreeConstants+4,zero,1))) {
                        ++failures_;stereo_=false;break;
                    }
                }
            }
        }
        if(modifyHud) {
            const Matrix& adjustment=math->hud[eye];
            // DP4 needs one column in each float4 constant. Replace the complete
            // patch for the next eye and restore it before the desktop draw.
            Matrix columns=transpose(adjustment);saved.hudChanged=true;
            RECT scissor=hudScissor(saved.scissor,saved.viewport,adjustment);
            if(headPanel) {
                scissor.left=std::min(scissor.left,saved.scissor.left);scissor.top=std::min(scissor.top,saved.scissor.top);
                scissor.right=std::max(scissor.right,saved.scissor.right);scissor.bottom=std::max(scissor.bottom,saved.scissor.bottom);
            }
            float panel[4]={2*config().headPanelLeft-1,1-2*config().headPanelTop,headPanel?1.0f:0.0f,0};
            if(FAILED(device_->SetVertexShader(shaderEntry->hud.Get())) ||
                FAILED(device_->SetVertexShaderConstantF(kHudConstants,columns.m,4)) ||
                FAILED(device_->SetVertexShaderConstantF(kHudConstants+4,panel,1)) ||
                FAILED(device_->SetRenderState(D3DRS_ZENABLE,D3DZB_FALSE)) ||
                FAILED(device_->SetRenderState(D3DRS_ZWRITEENABLE,FALSE)) ||
                (scissorEnabled && FAILED(device_->SetScissorRect(&scissor)))) {
                ++failures_;stereo_=false;break;
            }
        }
        if(FAILED(operation())){++failures_;stereo_=false;break;}
        if(modifyHud)++hudEyes;
        if(modifyTree)++treeEyes;
    }
    if(transformed) {++transformed_;if(info.effect)++effectDraws_;}
    if(hudEyes==2)++hudDraws_;
    if(treeEyes==2)++treeDraws_;
}
void Stereo::clear(DWORD count,const D3DRECT* rects,DWORD flags,D3DCOLOR color,float z,DWORD stencil) {
    startFrame();if(!stereo_ || recording_)return;
    EyeState saved;if(!saveEyeState(saved,false,0,false)) {++failures_;stereo_=false;return;}
    struct Restore {std::function<void()> action;~Restore(){action();}};
    Restore restore{[&]{restoreEyeState(saved);}};
    for(int eye=0;eye<2;++eye) {
        if(!bindEye(eye,saved) || FAILED(device_->Clear(count,rects,flags,color,z,stencil))) {++failures_;stereo_=false;break;}
    }
}
void Stereo::stretch(IDirect3DSurface9* source,const RECT* sourceRect,IDirect3DSurface9* target,const RECT* targetRect,D3DTEXTUREFILTERTYPE filter) {
    startFrame();if(!stereo_ || !source || !target || recording_)return;
    for(int eye=0;eye<2;++eye) {
        auto src=cloneSurface(source,eye),dst=cloneSurface(target,eye);
        if(!src || !dst || FAILED(device_->StretchRect(src.Get(),sourceRect,dst.Get(),targetRect,filter)))++failures_;
    }
}
void Stereo::fill(IDirect3DSurface9* target,const RECT* rect,D3DCOLOR color) {
    startFrame();if(!stereo_ || !target || recording_)return;
    for(int eye=0;eye<2;++eye) {auto dst=cloneSurface(target,eye);if(!dst || FAILED(device_->ColorFill(dst.Get(),rect,color)))++failures_;}
}
void Stereo::updateTexture(IDirect3DBaseTexture9* source,IDirect3DBaseTexture9* destination) {
    if(!stereo_)return;
    auto dst=textures_.find(destination);if(dst==textures_.end())return;
    auto src=textures_.find(source);
    for(int eye=0;eye<2;++eye)if(FAILED(device_->UpdateTexture(src==textures_.end()?source:src->second.eyes[eye].Get(),dst->second.eyes[eye].Get())))++failures_;
}
void Stereo::updateSurface(IDirect3DSurface9* source,const RECT* rect,IDirect3DSurface9* destination,const POINT* point) {
    if(!stereo_)return;
    auto dst=surfaces_.find(destination);if(dst==surfaces_.end())return;
    auto src=surfaces_.find(source);
    for(int eye=0;eye<2;++eye)if(FAILED(device_->UpdateSurface(src==surfaces_.end()?source:src->second.eyes[eye].Get(),rect,dst->second.eyes[eye].Get(),point)))++failures_;
}
void Stereo::renderTarget(DWORD index,IDirect3DSurface9* surface) {
    if(recording_ || !surface || index!=0 || surfaces_.find(surface)!=surfaces_.end())return;
    if(stereo_)cloneSurface(surface,0);
}
bool Stereo::conservativeVisibility() const {
    Tracking tracking{};
    return config().headAwareCulling && knownExecutable() && mapping_ && hostAlive(mapping_->get()) &&
        readTracking(mapping_->get(),tracking) && tracking.valid && tracking.stereoRequested;
}
bool Stereo::capture(IDirect3DSurface9* source,uint8_t* pixels) {
    if(!source)return false;
    D3DSURFACE_DESC d{};if(FAILED(source->GetDesc(&d)))return false;
    if(!d.Width || !d.Height || d.Width>kWidth || d.Height>kHeight)return false;
    uint32_t w=d.Width,h=d.Height;
    if(w!=captureWidth_ || h!=captureHeight_ || !resolve_ || !readback_) {
        resolve_.Reset();readback_.Reset();msaaResolve_.Reset();captureWidth_=w;captureHeight_=h;
        if(FAILED(device_->CreateRenderTarget(w,h,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,resolve_.GetAddressOf(),nullptr)) ||
            FAILED(device_->CreateOffscreenPlainSurface(w,h,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,readback_.GetAddressOf(),nullptr)))return false;
    }
    Ptr<IDirect3DSurface9> input(source);
    if(d.MultiSampleType!=D3DMULTISAMPLE_NONE) {
        D3DSURFACE_DESC old{};if(msaaResolve_)msaaResolve_->GetDesc(&old);
        if(!msaaResolve_ || old.Width!=d.Width || old.Height!=d.Height || old.Format!=d.Format) {
            msaaResolve_.Reset();
            if(FAILED(device_->CreateRenderTarget(d.Width,d.Height,d.Format,D3DMULTISAMPLE_NONE,0,FALSE,msaaResolve_.GetAddressOf(),nullptr)))return false;
        }
        if(FAILED(device_->StretchRect(source,nullptr,msaaResolve_.Get(),nullptr,D3DTEXF_NONE)))return false;
        input=msaaResolve_;
    }
    if(FAILED(device_->StretchRect(input.Get(),nullptr,resolve_.Get(),nullptr,(w==d.Width && h==d.Height)?D3DTEXF_NONE:D3DTEXF_LINEAR)))return false;
    if(FAILED(device_->GetRenderTargetData(resolve_.Get(),readback_.Get())))return false;
    D3DLOCKED_RECT locked{};if(FAILED(readback_->LockRect(&locked,nullptr,D3DLOCK_READONLY)))return false;
    for(uint32_t row=0;row<h;++row) {
        auto dst=pixels+size_t(row)*w*4;
        std::memcpy(dst,static_cast<const uint8_t*>(locked.pBits)+size_t(row)*locked.Pitch,size_t(w)*4);
    }
    readback_->UnlockRect();return true;
}
bool Stereo::queueImage(IDirect3DSurface9* source,Ptr<IDirect3DSurface9>& destination,uint32_t width,uint32_t height) {
    D3DSURFACE_DESC description{};
    if(!source || FAILED(source->GetDesc(&description)))return false;
    if(width!=description.Width || height!=description.Height || width>kWidth || height>kHeight)return false;
    D3DSURFACE_DESC previous{};if(destination)destination->GetDesc(&previous);
    if(destination && (previous.Width!=width || previous.Height!=height))destination.Reset();
    if(!destination) {
        // A standalone, non-MSAA lockable RT supports DONOTWAIT. RT textures
        // do not. Reading this directly removes GetRenderTargetData's driver wait.
        HRESULT hr=device_->CreateRenderTarget(width,height,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,TRUE,
            destination.GetAddressOf(),nullptr);
        if(FAILED(hr)) {deferredUnavailable_=true;log("lockable native capture %ux%u unavailable hr=%08x; no blocking fallback",width,height,hr);return false;}
        log("nonblocking capture surface %ux%u",width,height);
    }
    Ptr<IDirect3DSurface9> input(source);
    if(description.MultiSampleType!=D3DMULTISAMPLE_NONE) {
        D3DSURFACE_DESC old{};if(msaaResolve_)msaaResolve_->GetDesc(&old);
        if(!msaaResolve_ || old.Width!=description.Width || old.Height!=description.Height || old.Format!=description.Format) {
            msaaResolve_.Reset();
            if(FAILED(device_->CreateRenderTarget(description.Width,description.Height,description.Format,D3DMULTISAMPLE_NONE,0,FALSE,
                msaaResolve_.GetAddressOf(),nullptr)))return false;
        }
        if(FAILED(device_->StretchRect(source,nullptr,msaaResolve_.Get(),nullptr,D3DTEXF_NONE)))return false;
        input=msaaResolve_;
    }
    return SUCCEEDED(device_->StretchRect(input.Get(),nullptr,destination.Get(),nullptr,
        width==description.Width && height==description.Height?D3DTEXF_NONE:D3DTEXF_LINEAR));
}
void Stereo::releaseCapture(CaptureSlot& slot) {
    for(unsigned eye=0;eye<2;++eye)if(slot.mapped[eye]) {
        slot.color[eye]->UnlockRect();slot.mapped[eye]=false;slot.locked[eye]={};
    }
    slot.pending=slot.gpuComplete=false;
}
HRESULT Stereo::mapImages(CaptureSlot& slot) {
    bool busy=false;unsigned count=slot.info.stereo?2:1;
    // Poll every eye, even if the other is busy. Keep completed maps instead
    // of unlocking/restarting eye zero when eye one needs a later Present.
    // The slot is immutable until both maps complete and it is consumed.
    // NOSYSLOCK avoids holding a display-wide critical section across frames.
    for(unsigned eye=0;eye<count;++eye) {
        if(slot.mapped[eye])continue;
        HRESULT hr=slot.color[eye]->LockRect(&slot.locked[eye],nullptr,D3DLOCK_READONLY|D3DLOCK_DONOTWAIT|D3DLOCK_NOSYSLOCK);
        if(hr==D3DERR_WASSTILLDRAWING) {++readbackBusy_;busy=true;continue;}
        if(FAILED(hr)) {log("nonblocking readback failed eye=%u hr=%08x",eye,hr);return hr;}
        slot.mapped[eye]=true;
        if(!slot.locked[eye].pBits || slot.locked[eye].Pitch<static_cast<INT>(slot.info.width*4))return D3DERR_INVALIDCALL;
    }
    return busy?S_FALSE:S_OK;
}
HRESULT Stereo::readImages(CaptureSlot& slot,Frame& frame) {
    unsigned count=slot.info.stereo?2:1;
    for(unsigned eye=0;eye<count;++eye)if(!slot.mapped[eye])return D3DERR_INVALIDCALL;
    for(unsigned eye=0;eye<count;++eye)for(uint32_t row=0;row<slot.info.height;++row)
        std::memcpy(frame.pixels[eye]+size_t(row)*slot.info.width*4,
            static_cast<const uint8_t*>(slot.locked[eye].pBits)+size_t(row)*slot.locked[eye].Pitch,size_t(slot.info.width)*4);
    // The mono layer uses only eye zero; there is no second-eye CPU copy.
    return S_OK;
}
Stereo::FrameInfo Stereo::frameInfo(bool ready) const {
    FrameInfo info;info.tracking=tracking_;info.draws=draws_;info.transformed=transformed_;
    info.hud=hudDraws_;info.failures=failures_;info.queries=querySkipped_;info.stereo=ready;info.projection=projectionValid_;
    info.width=width_;info.height=height_;
    return info;
}
void Stereo::stampFrame(Frame& frame,const FrameInfo& info,float transfer) {
    frame.serial=InterlockedIncrement(&mapping_->get()->frameSerial);
    frame.width=info.width;frame.height=info.height;frame.stereo=info.stereo;
    frame.transformedDraws=info.transformed;frame.totalDraws=info.draws;frame.cloneFailures=info.failures;
    frame.querySkippedDraws=info.queries;frame.projectionValid=info.projection;frame.hudDraws=info.hud;
    frame.originGeneration=info.tracking.originGeneration;frame.displayTime=info.tracking.displayTime;
    std::memcpy(frame.eyes,info.tracking.eyes,sizeof(frame.eyes));std::memcpy(frame.fovs,info.tracking.fovs,sizeof(frame.fovs));
    frame.sourceFormat=D3DFMT_A8R8G8B8;frame.transferMilliseconds=transfer;
}
bool Stereo::deferredPresent(bool ready) {
    if(deferredUnavailable_)return true; // Never silently reintroduce a blocking transfer.
    if(draws_)++renderSamples_;
    double elapsed=0;bool captured=false;
    {
        Elapsed timer(elapsed);
        CaptureSlot* newest=nullptr;
        // Poll once per slot without FLUSH or a wait loop. Normal game Present
        // submits GPU commands; busy slots simply wait until another frame.
        for(auto& slot:captures_)if(slot.pending) {
            if(!slot.gpuComplete) {
                HRESULT result=slot.complete->GetData(nullptr,0,0);
                if(FAILED(result)) {releaseCapture(slot);continue;}
                if(result!=S_OK)continue;
                slot.gpuComplete=true;
            }
            HRESULT result=mapImages(slot);
            if(FAILED(result)) {releaseCapture(slot);continue;}
            if(result!=S_OK)continue;
            if(slot.order<=publishedOrder_) {releaseCapture(slot);continue;}
            if(!newest || slot.order>newest->order)newest=&slot;
        }
        if(newest) {
            Frame* frame=claimWrite(mapping_->get());
            if(frame) {
                struct Release {Frame* frame;~Release(){if(frame)InterlockedExchange(&frame->state,0);}} release{frame};
                double transfer=0;
                {
                    Elapsed timer(transfer);
                    captured=readImages(*newest,*frame)==S_OK;
                }
                if(captured) {
                    stampFrame(*frame,newest->info,static_cast<float>(transfer));publish(frame);release.frame=nullptr;
                    publishedOrder_=newest->order;++captureSamples_;captureAgeTotal_+=GetTickCount()-newest->queuedAt;
                }
                if(transfer>50)log("slow readback transfer=%.1fms stereo=%d draws=%u",transfer,newest->info.stereo,newest->info.draws);
            }
            for(auto& slot:captures_)if(slot.pending && slot.order<=publishedOrder_ && slot.gpuComplete &&
                slot.mapped[0] && (!slot.info.stereo || slot.mapped[1])) {
                // Recycle only fully finished slots. Older maps still in
                // progress get their own completion before reuse next time.
                releaseCapture(slot);
            }
        }
        if(draws_) {
            auto free=std::find_if(captures_.begin(),captures_.end(),[](const CaptureSlot& slot){return !slot.pending;});
            if(free==captures_.end())++captureDropped_;
            else {
                auto& slot=*free;
                if(!slot.complete && FAILED(device_->CreateQuery(D3DQUERYTYPE_EVENT,slot.complete.GetAddressOf()))) {
                    deferredUnavailable_=true;log("deferred capture event query unavailable; no blocking fallback");return true;
                }
                auto info=frameInfo(ready);bool copied=false;
                if(ready) {
                    auto left=cloneSurface(backbuffer_.Get(),0),right=cloneSurface(backbuffer_.Get(),1);
                    copied=left && right && queueImage(left.Get(),slot.color[0],info.width,info.height) &&
                        queueImage(right.Get(),slot.color[1],info.width,info.height);
                }
                if(!copied) {info.stereo=false;copied=queueImage(backbuffer_.Get(),slot.color[0],info.width,info.height);}
                if(copied && SUCCEEDED(slot.complete->Issue(D3DISSUE_END))) {
                    slot.info=info;slot.order=++captureOrder_;slot.pending=true;slot.gpuComplete=false;
                    slot.queuedAt=GetTickCount();++queueSamples_;
                } else {++failures_;log("capture queue failed; keeping the last completed VR frame");}
            }
        }
    }
    if(GetTickCount()-lastStatus_>3000) {
        DWORD now=GetTickCount();double seconds=(now-lastStatus_)/1000.0;lastStatus_=now;
        HWND foreground=GetForegroundWindow();DWORD foregroundPid=0;GetWindowThreadProcessId(foreground,&foregroundPid);
        log("frame captured=%d queuedStereo=%d native=%ux%u draws=%u transformed=%u effects=%u HUD=%u trees=%u projection=%d cloneFailures=%u querySkipped=%u scissorExpanded=%u replay=%.1fms captureWork=%.1fms dropped=%u readbackBusy=%u renderFPS=%.1f captureFPS=%.1f queueFPS=%.1f snapshotAge=%.1fms foregroundPid=%lu",
            captured,ready,width_,height_,draws_,transformed_,effectDraws_,hudDraws_,treeDraws_,projectionValid_,failures_,querySkipped_,scissorExpanded_,
            replayMilliseconds_,elapsed,captureDropped_,readbackBusy_,renderSamples_/seconds,captureSamples_/seconds,queueSamples_/seconds,
            captureSamples_?captureAgeTotal_/captureSamples_:0,foregroundPid);
        renderSamples_=captureSamples_=queueSamples_=0;captureAgeTotal_=0;
    }
    return true;
}
void Stereo::presented(HRESULT result) {
    static HRESULT previous=S_OK;
    if(result!=previous) {log("original Present status %08x -> %08x",previous,result);previous=result;}
}
void Stereo::present() {
    startFrame();
    if(!mapping_ || !hostAlive(mapping_->get()) || !refreshBackbuffer()) {started_=false;return;}
    if(FAILED(device_->TestCooperativeLevel())) {started_=false;return;}
    if(width_>kWidth || height_>kHeight) {started_=false;return;}
    bool ready=stereo_ && projectionValid_ && transformed_>0 && failures_==0 && querySkipped_==0;
    if(config().deferredCapture && deferredPresent(ready)) {started_=false;return;}
    // Extra Present calls and lost-device intervals contain no newly rendered
    // scene. Do not overwrite a valid stereo frame with a duplicate screen.
    if(!draws_) {started_=false;return;}
    Frame* frame=claimWrite(mapping_->get());
    if(!frame) {started_=false;return;}
    struct ReleaseSlot {
        Frame* owned;
        ~ReleaseSlot(){if(owned)InterlockedExchange(&owned->state,0);}
    } release{frame};
    LARGE_INTEGER start{},end{},frequency{};QueryPerformanceCounter(&start);QueryPerformanceFrequency(&frequency);
    bool captured=false;
    if(ready) {
        auto first=cloneSurface(backbuffer_.Get(),0),second=cloneSurface(backbuffer_.Get(),1);
        captured=first && second && capture(first.Get(),frame->pixels[0]) && capture(second.Get(),frame->pixels[1]);
    }
    if(!captured) {ready=false;captured=capture(backbuffer_.Get(),frame->pixels[0]);
        if(captured)std::memcpy(frame->pixels[1],frame->pixels[0],size_t(captureWidth_)*captureHeight_*4);
    }
    if(captured) {
        frame->serial=InterlockedIncrement(&mapping_->get()->frameSerial);
        frame->width=captureWidth_;frame->height=captureHeight_;frame->stereo=ready;
        frame->transformedDraws=transformed_;frame->totalDraws=draws_;frame->cloneFailures=failures_;
        frame->querySkippedDraws=querySkipped_;frame->projectionValid=projectionValid_;frame->originGeneration=tracking_.originGeneration;
        frame->hudDraws=hudDraws_;
        frame->displayTime=tracking_.displayTime;std::memcpy(frame->eyes,tracking_.eyes,sizeof(frame->eyes));std::memcpy(frame->fovs,tracking_.fovs,sizeof(frame->fovs));
        frame->sourceFormat=D3DFMT_A8R8G8B8;
        QueryPerformanceCounter(&end);frame->transferMilliseconds=1000.0f*float(end.QuadPart-start.QuadPart)/float(frequency.QuadPart);
        publish(frame);release.owned=nullptr;
    }
    if(GetTickCount()-lastStatus_>3000) {
        lastStatus_=GetTickCount();
        log("frame captured=%d requested=%d stereo=%d draws=%u transformed=%u effects=%u HUD=%u trees=%u projection=%d cloneFailures=%u querySkipped=%u replay=%.1fms transfer=%.1fms",
            captured,tracking_.stereoRequested,ready,draws_,transformed_,effectDraws_,hudDraws_,treeDraws_,projectionValid_,failures_,querySkipped_,replayMilliseconds_,captured?frame->transferMilliseconds:0);
    }
    started_=false;
}
}
