#pragma once
#include "vr_math.h"
#include <windows.h>
#include <d3d9.h>
#include <wrl/client.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <memory>
#include <string>

namespace acah {
template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
void log(const char* format, ...);
struct Config {
    bool enabled=true, transposeWvp=false, allowUnknown=false,hudEnabled=true,deferredCapture=true,headAwareCulling=true;
    float units=1, fallbackFov=0,hudDepth=0,headPanelLeft=0.72f,headPanelTop=0.72f;
    uint32_t maxPairs=96, maxMegabytes=256;
};
const Config& config();
void initializeHook();
void initializeVisibility();
std::shared_ptr<Mapping> gameMapping();
bool knownExecutable();
void dumpRuntimeObjects();
struct ProjectionObservation { Matrix projection=identity(); uint32_t serial=0; };
ProjectionObservation projectionObservation();
void observeProjection(const Matrix& m, const char* source);
void setGameAspect(float aspect);
float gameAspect();

struct ShaderInfo {
    int wvp=-1, view=-1, local=-1, camera=-1,hudTransform=-1;
    int inverseView=-1,inverseViewCount=0,inverseProjection=-1,globalCamera=-1;
    bool effect=false;
    uint32_t hash=0, bytes=0,vertexSamplers=0;
};
ShaderInfo inspectShader(IDirect3DVertexShader9* shader);
}
