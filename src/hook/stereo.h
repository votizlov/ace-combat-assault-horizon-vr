#pragma once
#include "support.h"
#include <functional>
#include <array>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace acah {
class Stereo {
    struct ConstantState {UINT index=0,count=0;Matrix original{};};
    struct EyeState {
        std::array<Ptr<IDirect3DSurface9>,4> targets;
        Ptr<IDirect3DSurface9> depth;
        D3DVIEWPORT9 viewport{};
        RECT scissor{};
        std::array<Ptr<IDirect3DBaseTexture9>,20> textures;
        std::array<bool,20> textureValid{},textureChanged{};
        Ptr<IDirect3DVertexBuffer9> stream;
        Ptr<IDirect3DIndexBuffer9> indices;
        Ptr<IDirect3DVertexShader9> vertex;
        std::array<float,20> hudConstants{};
        std::array<ConstantState,5> sceneConstants{};
        unsigned sceneConstantCount=0;
        DWORD zEnabled=0,zWrite=0;
        uint32_t pixelSamplers=0;
        bool pixelSamplersValid=false;
        UINT offset=0,stride=0;
        bool userMemory=false,targetsChanged=false,wvpChanged=false,hudChanged=false,treeChanged=false,sceneChanged=false;
        int wvpRegister=-1;
        Matrix wvp{};
    };
    struct SurfacePair { Ptr<IDirect3DSurface9> original, eyes[2]; };
    struct TexturePair { Ptr<IDirect3DTexture9> original, eyes[2]; };
    struct ShaderEntry {
        Ptr<IDirect3DVertexShader9> original,hud,tree;
        ShaderInfo info;
        bool hudAttempted=false,treeAttempted=false;
    };
    struct PixelEntry { Ptr<IDirect3DPixelShader9> original;uint32_t samplers=0;bool valid=false; };
    struct FrameInfo {
        Tracking tracking{};
        uint32_t draws=0,transformed=0,hud=0,failures=0,queries=0,width=0,height=0;
        bool stereo=false,projection=false;
    };
    struct CaptureSlot {
        Ptr<IDirect3DSurface9> color[2];
        Ptr<IDirect3DQuery9> complete;
        std::array<D3DLOCKED_RECT,2> locked{};
        std::array<bool,2> mapped{};
        FrameInfo info{};
        uint64_t order=0;
        DWORD queuedAt=0;
        bool pending=false,gpuComplete=false;
    };
    struct EyeMath {
        Matrix native{},inverseProjection{},inverseRelative[2]{},relative[2]{},projection[2]{},correction[2]{},hud[2]{};
        bool present=false,valid=false,hudValid=false;
    };
    IDirect3DDevice9* device_;
    std::shared_ptr<Mapping> mapping_;
    std::unordered_map<IDirect3DSurface9*,SurfacePair> surfaces_;
    std::unordered_map<IDirect3DBaseTexture9*,TexturePair> textures_;
    std::unordered_map<IDirect3DVertexShader9*,ShaderEntry> shaders_;
    std::unordered_map<IDirect3DPixelShader9*,PixelEntry> pixelShaders_;
    std::unordered_set<uint32_t> sampledShaders_;
    std::unordered_set<uint32_t> sampledHudShaders_;
    Ptr<IDirect3DSurface9> backbuffer_, resolve_, readback_, msaaResolve_;
    std::array<CaptureSlot,3> captures_;
    uint64_t captureOrder_=0,publishedOrder_=0;
    uint32_t captureDropped_=0,readbackBusy_=0,scissorExpanded_=0;
    uint32_t renderSamples_=0,captureSamples_=0,queueSamples_=0;
    double captureAgeTotal_=0;
    std::array<EyeMath,2> eyeMath_;
    unsigned eyeMathNext_=0;
    bool deferredUnavailable_=false;
    Tracking tracking_{};
    uint32_t width_=0,height_=0,captureWidth_=0,captureHeight_=0;
    uint64_t cloneBytes_=0;
    uint32_t draws_=0,transformed_=0,effectDraws_=0,hudDraws_=0,treeDraws_=0,failures_=0,querySkipped_=0,activeQueries_=0;
    double replayMilliseconds_=0;
    Matrix sceneView_=identity();
    bool sceneViewValid_=false;
    bool started_=false,stereo_=false,recording_=false;
    uint32_t lastStatus_=GetTickCount(),projectionSerial_=0;
    Matrix projection_=identity();
    bool projectionValid_=false;
    bool refreshBackbuffer();
    void startFrame();
    bool saveEyeState(EyeState& state,bool userMemory,uint32_t vertexSamplers=0xf0000,bool textures=true);
    bool restoreEyeState(EyeState& state);
    bool bindEye(int eye,EyeState& state);
    const EyeMath& eyeMath(const Matrix& projection);
    Ptr<IDirect3DSurface9> cloneSurface(IDirect3DSurface9* source,int eye);
    bool capture(IDirect3DSurface9* source,uint8_t* pixels);
    bool queueImage(IDirect3DSurface9* source,Ptr<IDirect3DSurface9>& destination,uint32_t width,uint32_t height);
    void releaseCapture(CaptureSlot& slot);
    HRESULT mapImages(CaptureSlot& slot);
    HRESULT readImages(CaptureSlot& slot,Frame& frame);
    FrameInfo frameInfo(bool ready) const;
    void stampFrame(Frame& frame,const FrameInfo& info,float transfer);
    bool deferredPresent(bool ready);
public:
    explicit Stereo(IDirect3DDevice9* device);
    ~Stereo();
    void reset();
    void rejectFrame() noexcept { started_=true;stereo_=false;++failures_; }
    void begin() { startFrame(); }
    void present();
    void shader(IDirect3DVertexShader9* shader);
    void draw(const std::function<HRESULT()>& operation,bool userMemory=false,UINT primitiveCount=0);
    void clear(DWORD count,const D3DRECT* rects,DWORD flags,D3DCOLOR color,float z,DWORD stencil);
    void stretch(IDirect3DSurface9* source,const RECT* sourceRect,IDirect3DSurface9* target,const RECT* targetRect,D3DTEXTUREFILTERTYPE filter);
    void fill(IDirect3DSurface9* target,const RECT* rect,D3DCOLOR color);
    void updateTexture(IDirect3DBaseTexture9* source,IDirect3DBaseTexture9* destination);
    void updateSurface(IDirect3DSurface9* source,const RECT* rect,IDirect3DSurface9* destination,const POINT* point);
    void recording(bool value) { recording_=value; }
    void query(bool begin) { if(begin)++activeQueries_;else if(activeQueries_)--activeQueries_; }
    bool conservativeVisibility() const;
    void renderTarget(DWORD index,IDirect3DSurface9* surface);
    void presented(HRESULT result);
};
}
