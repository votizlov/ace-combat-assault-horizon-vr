#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include "vr_math.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
void xrCheck(XrResult result,const char* operation) {
    if(XR_FAILED(result))throw std::runtime_error(std::string(operation)+" failed: OpenXR result "+std::to_string(result));
}
void hrCheck(HRESULT result,const char* operation) {
    if(FAILED(result))throw std::runtime_error(std::string(operation)+" failed: HRESULT "+std::to_string(static_cast<uint32_t>(result)));
}
acah::Pose toWire(const XrPosef& pose) {
    return {{pose.orientation.x,pose.orientation.y,pose.orientation.z,pose.orientation.w},
        {pose.position.x,pose.position.y,pose.position.z},0};
}
XrPosef fromWire(const acah::Pose& pose) {
    return {{pose.orientation[0],pose.orientation[1],pose.orientation[2],pose.orientation[3]},
        {pose.position[0],pose.position[1],pose.position[2]}};
}
acah::Fov toWire(const XrFovf& fov) {return {fov.angleLeft,fov.angleRight,fov.angleUp,fov.angleDown};}
XrFovf fromWire(const acah::Fov& fov) {return {fov.left,fov.right,fov.up,fov.down};}

struct App {
    acah::Mapping mapping;
    XrInstance instance=XR_NULL_HANDLE;
    XrSystemId system=XR_NULL_SYSTEM_ID;
    XrSession session=XR_NULL_HANDLE;
    XrSpace local=XR_NULL_HANDLE,viewSpace=XR_NULL_HANDLE;
    std::array<XrSwapchain,2> swapchains{{XR_NULL_HANDLE,XR_NULL_HANDLE}};
    std::array<std::vector<XrSwapchainImageD3D11KHR>,2> images;
    std::array<std::vector<uint32_t>,2> uploadedSerials;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    std::unique_ptr<acah::Frame> last=std::make_unique<acah::Frame>();
    std::vector<uint8_t> swizzled;
    int64_t colorFormat=0;
    bool rgba=false,running=false,quit=false,stereoRequested=false,originValid=false;
    bool haveFrame=false,screenOnly=false;
    uint32_t lastArrival=0,lastStatus=GetTickCount();
    uint32_t xrSamples=0,receivedSamples=0,submitSamples=0;
    std::vector<XrTime> referenceChanges;
    acah::Tracking tracking{};
    uint32_t swapWidth=acah::kWidth,swapHeight=acah::kHeight;
    std::atomic<bool> stopHeartbeat{false};
    std::atomic<const char*> activeStage{"initialization"};
    std::atomic<DWORD> stageStarted{GetTickCount()};
    std::thread heartbeat;
    LONG observedGamePid=0;
    HANDLE gameProcess=nullptr;
    bool staleReported=false;
    struct Submitted {
        uint32_t serial=0,width=0,height=0,generation=0;
        bool stereo=false,valid=false;
        acah::Pose eyes[2]{};
        acah::Fov fovs[2]{};
    } submitted;
    std::array<bool,2> acquired{},waited{};
    std::array<uint32_t,2> acquiredIndex{};
    uint32_t swapchainBusy=0;

    void stage(const char* name) {stageStarted.store(GetTickCount());activeStage.store(name);}
    void finished(const char* name,DWORD start) {
        DWORD elapsed=GetTickCount()-start;
        if(elapsed>100)printf("Slow host operation %s: %lums\n",name,elapsed);
        stage("between frames");
    }
    bool gameAlive() {
        LONG pid=InterlockedCompareExchange(&mapping.get()->gamePid,0,0);
        if(pid!=observedGamePid) {
            if(gameProcess)CloseHandle(gameProcess);
            observedGamePid=pid;gameProcess=pid?OpenProcess(SYNCHRONIZE,FALSE,static_cast<DWORD>(pid)):nullptr;
            haveFrame=false;submitted.valid=false;staleReported=false;
        }
        if(!pid)return false;
        if(gameProcess && WaitForSingleObject(gameProcess,0)==WAIT_OBJECT_0) {
            InterlockedCompareExchange(&mapping.get()->gamePid,0,pid);haveFrame=false;return false;
        }
        return true;
    }

    App() {
        auto c=mapping.get();LONG me=static_cast<LONG>(GetCurrentProcessId());
        LONG previous=InterlockedCompareExchange(&c->hostPid,me,0);
        if(previous && previous!=me) {
            HANDLE process=OpenProcess(SYNCHRONIZE,FALSE,static_cast<DWORD>(previous));
            DWORD error=process?ERROR_SUCCESS:GetLastError();
            bool alive=process && WaitForSingleObject(process,0)==WAIT_TIMEOUT;
            if(process)CloseHandle(process);
            if(alive || (!process && error!=ERROR_INVALID_PARAMETER))
                throw std::runtime_error("Another VR host owns this channel; close it before starting a new host");
            if(InterlockedCompareExchange(&c->hostPid,me,previous)!=previous)
                throw std::runtime_error("Another VR host acquired the channel during startup");
        }
        // Recover an odd sequence if the previous host crashed during a write.
        // Heartbeat is cleared first so the game cannot consume a partial reset.
        InterlockedExchange(&c->hostHeartbeat,0);
        tracking.originGeneration=c->tracking.originGeneration+1;
        InterlockedExchange(&c->trackingSequence,0);
        acah::writeTracking(c,tracking);
        // OpenXR waits must not expire the game's host lease and cause a
        // screen/stereo feedback loop. Only the frame thread writes Tracking;
        // this worker updates the independent heartbeat and reports long waits.
        heartbeat=std::thread([this,c] {
            DWORD report=0;
            while(!stopHeartbeat.load()) {
                InterlockedExchange(&c->hostHeartbeat,static_cast<LONG>(GetTickCount()));
                DWORD elapsed=GetTickCount()-stageStarted.load();
                if(elapsed>1000 && GetTickCount()-report>3000) {
                    report=GetTickCount();printf("Host waiting in %s for %lums\n",activeStage.load(),elapsed);
                }
                Sleep(100);
            }
        });
    }
    ~App() {
        stopHeartbeat.store(true);if(heartbeat.joinable())heartbeat.join();
        if(gameProcess)CloseHandle(gameProcess);
        auto c=mapping.get();tracking.valid=0;tracking.stereoRequested=0;
        acah::writeTracking(c,tracking);InterlockedExchange(&c->hostHeartbeat,0);
        InterlockedCompareExchange(&c->hostPid,0,static_cast<LONG>(GetCurrentProcessId()));
        for(auto swap:swapchains)if(swap)xrDestroySwapchain(swap);
        if(viewSpace)xrDestroySpace(viewSpace);
        if(local)xrDestroySpace(local);
        if(session)xrDestroySession(session);
        if(instance)xrDestroyInstance(instance);
    }
    void initialize() {
        uint32_t count=0;
        xrCheck(xrEnumerateInstanceExtensionProperties(nullptr,0,&count,nullptr),"enumerate extensions");
        std::vector<XrExtensionProperties> extensions(count,{XR_TYPE_EXTENSION_PROPERTIES});
        xrCheck(xrEnumerateInstanceExtensionProperties(nullptr,count,&count,extensions.data()),"enumerate extensions");
        bool d3d=false;for(const auto& extension:extensions)if(!std::strcmp(extension.extensionName,XR_KHR_D3D11_ENABLE_EXTENSION_NAME))d3d=true;
        if(!d3d)throw std::runtime_error("Active OpenXR runtime does not expose XR_KHR_D3D11_enable");
        const char* names[]={XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
        XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
        strcpy_s(create.applicationInfo.applicationName,"ACAH VR proof of concept");
        create.applicationInfo.applicationVersion=1;strcpy_s(create.applicationInfo.engineName,"ACAH custom bridge");
        create.applicationInfo.apiVersion=XR_MAKE_VERSION(1,0,0);
        create.enabledExtensionCount=1;create.enabledExtensionNames=names;
        xrCheck(xrCreateInstance(&create,&instance),"create instance");
        XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};xrCheck(xrGetInstanceProperties(instance,&properties),"instance properties");
        printf("OpenXR runtime: %s\n",properties.runtimeName);
        if(!std::strstr(properties.runtimeName,"SteamVR"))printf("The active runtime is not named SteamVR; select SteamVR in its OpenXR settings if intended.\n");
        XrSystemGetInfo get{XR_TYPE_SYSTEM_GET_INFO};get.formFactor=XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        xrCheck(xrGetSystem(instance,&get,&system),"get headset system (start SteamVR and connect the headset)");
        PFN_xrGetD3D11GraphicsRequirementsKHR requirementsFn=nullptr;
        xrCheck(xrGetInstanceProcAddr(instance,"xrGetD3D11GraphicsRequirementsKHR",reinterpret_cast<PFN_xrVoidFunction*>(&requirementsFn)),"D3D11 requirements function");
        XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
        xrCheck(requirementsFn(instance,system,&requirements),"D3D11 graphics requirements");
        ComPtr<IDXGIFactory1> factory;hrCheck(CreateDXGIFactory1(__uuidof(IDXGIFactory1),reinterpret_cast<void**>(factory.GetAddressOf())),"DXGI factory");
        ComPtr<IDXGIAdapter1> adapter;
        for(UINT index=0;;++index) {
            ComPtr<IDXGIAdapter1> candidate;
            if(factory->EnumAdapters1(index,candidate.GetAddressOf())==DXGI_ERROR_NOT_FOUND)break;
            DXGI_ADAPTER_DESC1 description{};candidate->GetDesc1(&description);
            if(description.AdapterLuid.LowPart==requirements.adapterLuid.LowPart && description.AdapterLuid.HighPart==requirements.adapterLuid.HighPart) {adapter=candidate;break;}
        }
        if(!adapter)throw std::runtime_error("OpenXR required GPU adapter was not found");
        std::vector<D3D_FEATURE_LEVEL> levels={D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0,D3D_FEATURE_LEVEL_10_1,D3D_FEATURE_LEVEL_10_0};
        levels.erase(std::remove_if(levels.begin(),levels.end(),[&](auto f){return f<requirements.minFeatureLevel;}),levels.end());
        if(levels.empty())throw std::runtime_error("Runtime requires a D3D feature level outside this prototype's supported levels");
        hrCheck(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels.data(),static_cast<UINT>(levels.size()),D3D11_SDK_VERSION,device.GetAddressOf(),nullptr,context.GetAddressOf()),"create D3D11 device");
        XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};binding.device=device.Get();
        XrSessionCreateInfo sessionCreate{XR_TYPE_SESSION_CREATE_INFO};sessionCreate.next=&binding;sessionCreate.systemId=system;
        xrCheck(xrCreateSession(instance,&sessionCreate,&session),"create session");
        XrReferenceSpaceCreateInfo space{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};space.poseInReferenceSpace.orientation.w=1;
        space.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_LOCAL;xrCheck(xrCreateReferenceSpace(session,&space,&local),"create local space");
        space.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_VIEW;xrCheck(xrCreateReferenceSpace(session,&space,&viewSpace),"create view space");
        xrCheck(xrEnumerateViewConfigurationViews(instance,system,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,0,&count,nullptr),"enumerate stereo views");
        if(count!=2)throw std::runtime_error("This prototype requires exactly two stereo views");
        std::vector<XrViewConfigurationView> configs(count,{XR_TYPE_VIEW_CONFIGURATION_VIEW});
        xrCheck(xrEnumerateViewConfigurationViews(instance,system,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,count,&count,configs.data()),"stereo view configuration");
        for(const auto& config:configs) {swapWidth=std::min(swapWidth,config.maxImageRectWidth);swapHeight=std::min(swapHeight,config.maxImageRectHeight);}
        if(swapWidth<acah::kWidth || swapHeight<acah::kHeight)throw std::runtime_error("Runtime maximum image size is below native 2560x1440; no automatic downscale");
        printf("VR transport v%u: native image capacity %ux%u, two IPC slots\n",acah::kVersion,acah::kWidth,acah::kHeight);
        uint32_t blendCount=0;xrCheck(xrEnumerateEnvironmentBlendModes(instance,system,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,0,&blendCount,nullptr),"enumerate blend modes");
        std::vector<XrEnvironmentBlendMode> blends(blendCount);
        xrCheck(xrEnumerateEnvironmentBlendModes(instance,system,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,blendCount,&blendCount,blends.data()),"blend modes");
        if(std::find(blends.begin(),blends.end(),XR_ENVIRONMENT_BLEND_MODE_OPAQUE)==blends.end())throw std::runtime_error("An opaque VR display is required");
        xrCheck(xrEnumerateSwapchainFormats(session,0,&count,nullptr),"enumerate swapchain formats");
        std::vector<int64_t> formats(count);xrCheck(xrEnumerateSwapchainFormats(session,count,&count,formats.data()),"swapchain formats");
        for(auto format:{DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM})
            if(std::find(formats.begin(),formats.end(),int64_t(format))!=formats.end()) {colorFormat=format;break;}
        if(!colorFormat)throw std::runtime_error("Runtime has no supported 8-bit color swapchain format");
        rgba=colorFormat==DXGI_FORMAT_R8G8B8A8_UNORM || colorFormat==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        printf("Swapchain color format=%lld CPU swizzle=%d\n",static_cast<long long>(colorFormat),rgba);
        for(int eye=0;eye<2;++eye) {
            XrSwapchainCreateInfo swap{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            swap.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT|XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
            swap.format=colorFormat;swap.sampleCount=1;swap.width=swapWidth;swap.height=swapHeight;swap.faceCount=1;swap.arraySize=1;swap.mipCount=1;
            xrCheck(xrCreateSwapchain(session,&swap,&swapchains[eye]),"create swapchain");
            xrCheck(xrEnumerateSwapchainImages(swapchains[eye],0,&count,nullptr),"enumerate swapchain images");
            images[eye].resize(count,{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
            uploadedSerials[eye].resize(count,0);
            xrCheck(xrEnumerateSwapchainImages(swapchains[eye],count,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(images[eye].data())),"swapchain images");
        }
        printf("Ready. F8 switches screen/stereo, F9 recenters, F10 returns to screen, Ctrl+Shift+F12 exits this host.\n");
    }
    void events() {
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        XrResult result;
        while((result=xrPollEvent(instance,&event))==XR_SUCCESS) {
            if(event.type==XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                auto state=reinterpret_cast<XrEventDataSessionStateChanged*>(&event)->state;
                printf("Session state: %d\n",state);
                if(state==XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};begin.primaryViewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    xrCheck(xrBeginSession(session,&begin),"begin session");running=true;
                }
                if(state==XR_SESSION_STATE_STOPPING) {xrCheck(xrEndSession(session),"end session");running=false;}
                if(state==XR_SESSION_STATE_EXITING || state==XR_SESSION_STATE_LOSS_PENDING)quit=true;
            } else if(event.type==XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING)quit=true;
            else if(event.type==XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
                auto change=reinterpret_cast<XrEventDataReferenceSpaceChangePending*>(&event);
                if(change->session==session && change->referenceSpaceType==XR_REFERENCE_SPACE_TYPE_LOCAL) {
                    referenceChanges.push_back(change->changeTime);
                    printf("Tracking reference change scheduled; recentering will follow at its effective time.\n");
                }
            }
            event={XR_TYPE_EVENT_DATA_BUFFER};
        }
        if(result!=XR_EVENT_UNAVAILABLE)xrCheck(result,"poll event");
    }
    void receiveFrame() {
        auto c=mapping.get();auto frame=acah::claimLatest(c);if(!frame)return;
        bool valid=frame->totalDraws && frame->width && frame->height && frame->width<=acah::kWidth && frame->height<=acah::kHeight && frame->sourceFormat==21;
        if(valid) {
            std::memcpy(last.get(),frame,offsetof(acah::Frame,pixels));
            size_t bytes=size_t(frame->width)*frame->height*4;
            std::memcpy(last->pixels[0],frame->pixels[0],bytes);
            if(frame->stereo)std::memcpy(last->pixels[1],frame->pixels[1],bytes);
            haveFrame=true;lastArrival=GetTickCount();++receivedSamples;
        }
        InterlockedExchange(&frame->state,0);
    }
    void upload(int eye,uint32_t imageIndex) {
        if(uploadedSerials[eye][imageIndex]==last->serial)return;
        const uint8_t* source=last->pixels[eye];
        size_t bytes=size_t(last->width)*last->height*4;
        if(rgba) {
            swizzled.resize(bytes);
            for(size_t i=0;i<bytes;i+=4){swizzled[i]=source[i+2];swizzled[i+1]=source[i+1];swizzled[i+2]=source[i];swizzled[i+3]=255;}
            source=swizzled.data();
        }
        D3D11_BOX box{0,0,0,last->width,last->height,1};
        context->UpdateSubresource(images[eye][imageIndex].texture,0,&box,source,last->width*4,0);
        uploadedSerials[eye][imageIndex]=last->serial;
    }
    void submitImages(bool immersive) {
        unsigned count=immersive?2:1;
        if(submitted.valid && submitted.serial==last->serial && submitted.stereo==immersive &&
            submitted.generation==tracking.originGeneration)return;
        bool ready=true;
        for(unsigned eye=0;eye<2;++eye) {
            if(eye>=count && !acquired[eye])continue;
            if(!acquired[eye]) {
                XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                xrCheck(xrAcquireSwapchainImage(swapchains[eye],&acquire,&acquiredIndex[eye]),"acquire image");
                acquired[eye]=true;waited[eye]=false;
            }
            if(!waited[eye]) {
                XrSwapchainImageWaitInfo waitImage{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};waitImage.timeout=0;
                stage("xrWaitSwapchainImage");DWORD start=GetTickCount();
                XrResult result=xrWaitSwapchainImage(swapchains[eye],&waitImage);
                finished("xrWaitSwapchainImage",start);
                if(result==XR_TIMEOUT_EXPIRED) {++swapchainBusy;ready=false;continue;}
                xrCheck(result,"wait image");waited[eye]=true;
            }
        }
        if(!ready)return;
        // Both eyes become writable before either is uploaded/released. On a
        // timeout keep ownership, retry that same index next frame, and compose
        // the previous released pair with its own pose/FOV. Never release an
        // image after XR_TIMEOUT_EXPIRED (it is a positive result, not success).
        for(unsigned eye=0;eye<count;++eye) {
            stage("D3D11 upload");DWORD start=GetTickCount();upload(eye,acquiredIndex[eye]);finished("D3D11 upload",start);
        }
        // Submit both uploads together; two flushes per stereo pair needlessly
        // split the work. Flush before release, as required by the runtime.
        stage("D3D11 pair flush");DWORD flushStart=GetTickCount();context->Flush();finished("D3D11 pair flush",flushStart);
        for(unsigned eye=0;eye<2;++eye)if(acquired[eye]) {
            XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            xrCheck(xrReleaseSwapchainImage(swapchains[eye],&release),"release image");acquired[eye]=waited[eye]=false;
        }
        submitted.serial=last->serial;submitted.width=last->width;submitted.height=last->height;
        submitted.generation=tracking.originGeneration;submitted.stereo=immersive;submitted.valid=true;
        ++submitSamples;
        std::memcpy(submitted.eyes,last->eyes,sizeof(submitted.eyes));std::memcpy(submitted.fovs,last->fovs,sizeof(submitted.fovs));
    }
    void run() {
        std::array<bool,256> keys{};
        for(int key : {VK_F8,VK_F9,VK_F10,VK_F12})keys[key]=(GetAsyncKeyState(key)&0x8000)!=0;
        auto pressed=[&](int key) {
            bool down=(GetAsyncKeyState(key)&0x8000)!=0;
            bool edge=down && !keys[key];keys[key]=down;return edge;
        };
        if(screenOnly){stereoRequested=false;printf("Screen-only diagnostic run: F8 is disabled. F9 recenters.\n");}
        while(!quit) {
            stage("xrPollEvent");DWORD eventStart=GetTickCount();events();finished("xrPollEvent",eventStart);
            if(pressed(VK_F12) && (GetAsyncKeyState(VK_CONTROL)&0x8000) && (GetAsyncKeyState(VK_SHIFT)&0x8000))quit=true;
            if(pressed(VK_F8) && !screenOnly){stereoRequested=!stereoRequested;printf("Requested mode: %s\n",stereoRequested?"experimental stereo":"virtual screen");}
            if(pressed(VK_F10)){stereoRequested=false;printf("Requested mode: virtual screen\n");}
            bool recenter=pressed(VK_F9);
            if(quit)break;
            if(!running) {
                stage("session idle");
                tracking.valid=0;tracking.stereoRequested=0;acah::writeTracking(mapping.get(),tracking);Sleep(10);continue;
            }
            XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};XrFrameState frame{XR_TYPE_FRAME_STATE};
            stage("xrWaitFrame");DWORD waitStart=GetTickCount();
            xrCheck(xrWaitFrame(session,&wait,&frame),"wait frame");
            finished("xrWaitFrame",waitStart);
            XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};xrCheck(xrBeginFrame(session,&begin),"begin frame");
            auto due=[&](XrTime time){return time<=frame.predictedDisplayTime;};
            if(std::any_of(referenceChanges.begin(),referenceChanges.end(),due)) {
                originValid=false;haveFrame=false;
                referenceChanges.erase(std::remove_if(referenceChanges.begin(),referenceChanges.end(),due),referenceChanges.end());
            }
            XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};locate.viewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            locate.displayTime=frame.predictedDisplayTime;locate.space=local;
            std::array<XrView,2> views{{{XR_TYPE_VIEW},{XR_TYPE_VIEW}}};
            XrViewState state{XR_TYPE_VIEW_STATE};uint32_t count=0;
            xrCheck(xrLocateViews(session,&locate,&state,2,&count,views.data()),"locate views");
            XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};xrCheck(xrLocateSpace(viewSpace,local,frame.predictedDisplayTime,&head),"locate headset center");
            auto viewFlags=XR_VIEW_STATE_ORIENTATION_VALID_BIT|XR_VIEW_STATE_POSITION_VALID_BIT;
            auto headFlags=XR_SPACE_LOCATION_ORIENTATION_VALID_BIT|XR_SPACE_LOCATION_POSITION_VALID_BIT;
            tracking.valid=count==2 && (state.viewStateFlags&viewFlags)==viewFlags && (head.locationFlags&headFlags)==headFlags;
            if(tracking.valid && (!originValid || recenter)) {
                tracking.origin=toWire(head.pose);originValid=true;++tracking.originGeneration;haveFrame=false;
                printf("Recentered seated origin, generation %u\n",tracking.originGeneration);
            }
            ++tracking.serial;tracking.displayTime=frame.predictedDisplayTime;tracking.stereoRequested=stereoRequested && frame.shouldRender;
            for(int eye=0;eye<2;++eye){tracking.eyes[eye]=toWire(views[eye].pose);tracking.fovs[eye]=toWire(views[eye].fov);}
            acah::writeTracking(mapping.get(),tracking);bool gameRunning=gameAlive();receiveFrame();
            uint32_t imageAge=GetTickCount()-lastArrival;
            bool stale=gameRunning && haveFrame && imageAge>500;
            if(stale!=staleReported) {
                printf(stale?"Game image stream stalled: age=%ums; retaining last projection\n":"Game image stream resumed: age=%ums\n",imageAge);
                staleReported=stale;
            }
            bool haveImage=frame.shouldRender && haveFrame && originValid && gameRunning;
            bool wantImmersive=haveImage && stereoRequested && tracking.valid && last->stereo &&
                last->originGeneration==tracking.originGeneration;
            if(haveImage)submitImages(wantImmersive);
            bool show=frame.shouldRender && originValid && gameRunning && submitted.valid && submitted.generation==tracking.originGeneration;
            bool immersive=show && submitted.stereo;
            std::array<XrCompositionLayerProjectionView,2> projectionViews{{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}}};
            XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            XrCompositionLayerQuad screen{XR_TYPE_COMPOSITION_LAYER_QUAD};
            const XrCompositionLayerBaseHeader* layer=nullptr;
            if(show) {
                for(int eye=0;eye<(immersive?2:1);++eye) {
                    auto& p=projectionViews[eye];p.pose=fromWire(submitted.eyes[eye]);p.fov=fromWire(submitted.fovs[eye]);
                    p.subImage.swapchain=swapchains[eye];p.subImage.imageRect.extent={static_cast<int32_t>(submitted.width),static_cast<int32_t>(submitted.height)};
                }
                if(immersive) {
                    projection.space=local;projection.viewCount=2;projection.views=projectionViews.data();
                    layer=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
                } else {
                    screen.space=local;screen.eyeVisibility=XR_EYE_VISIBILITY_BOTH;
                    screen.subImage.swapchain=swapchains[0];screen.subImage.imageRect.extent={static_cast<int32_t>(submitted.width),static_cast<int32_t>(submitted.height)};
                    screen.pose=fromWire(tracking.origin);
                    auto location=DirectX::XMVector3TransformCoord(DirectX::XMVectorSet(0,0,-2.5f,1),acah::load(acah::poseMatrix(tracking.origin)));
                    DirectX::XMFLOAT3 position;DirectX::XMStoreFloat3(&position,location);screen.pose.position={position.x,position.y,position.z};
                    screen.size={3.0f,3.0f*submitted.height/submitted.width};
                    layer=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&screen);
                }
            }
            XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.displayTime=frame.predictedDisplayTime;end.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            end.layerCount=layer?1:0;end.layers=layer?&layer:nullptr;
            stage("xrEndFrame");DWORD endStart=GetTickCount();xrCheck(xrEndFrame(session,&end),"end frame");finished("xrEndFrame",endStart);
            ++xrSamples;
            if(GetTickCount()-lastStatus>3000) {
                DWORD now=GetTickCount();double seconds=(now-lastStatus)/1000.0;lastStatus=now;
                if(haveFrame)printf("Game pid=%ld frame=%u mode=%s requested=%s native=%ux%u draws=%u WVP=%u HUD=%u projection=%u clonesFailed=%u queriesSkipped=%u transfer=%.1fms age=%ums submitted=%u swapchainBusy=%u\n",
                    mapping.get()->gamePid,last->serial,!show?"WAIT":immersive?"STEREO EXPERIMENT":"SCREEN",stereoRequested?"stereo":"screen",
                    last->width,last->height,last->totalDraws,last->transformedDraws,last->hudDraws,last->projectionValid,last->cloneFailures,last->querySkippedDraws,last->transferMilliseconds,GetTickCount()-lastArrival,submitted.serial,swapchainBusy);
                else printf("Waiting for game frames. Game pid=%ld; run the game with the x86 proxy installed.\n",mapping.get()->gamePid);
                printf("Host rates xrFPS=%.1f receivedFPS=%.1f submittedFPS=%.1f\n",xrSamples/seconds,receivedSamples/seconds,submitSamples/seconds);
                xrSamples=receivedSamples=submitSamples=0;
            }
        }
    }
};
}
int main(int argc,char** argv) {
    setvbuf(stdout,nullptr,_IONBF,0);
    try {
        App app;
        for(int i=1;i<argc;++i) {
            if(!std::strcmp(argv[i],"--stereo"))app.stereoRequested=true;
            else if(!std::strcmp(argv[i],"--screen"))app.stereoRequested=false;
            else if(!std::strcmp(argv[i],"--screen-only"))app.screenOnly=true;
            else {fprintf(stderr,"Usage: acah_vr_host.exe [--screen | --stereo | --screen-only]\n");return 2;}
        }
        app.initialize();app.run();return 0;
    } catch(const std::exception& e) {fprintf(stderr,"ACAH VR host: %s\n",e.what());return 1;}
}
