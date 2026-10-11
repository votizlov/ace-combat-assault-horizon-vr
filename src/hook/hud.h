#pragma once
#include "support.h"

namespace acah {
constexpr UINT kHudConstants=248,kHudConstantCount=5;
bool knownHudShader(uint32_t hash);
bool screenSpaceTransform(const Matrix& transform);
bool hudTextureSamplers(IDirect3DPixelShader9* shader,uint32_t& mask);
// Rewrite only the final POSITION output of an inspected VS 3.0 UI shader.
// The original text UVs, vertex colors and shader-defined constants stay intact.
Ptr<IDirect3DVertexShader9> createHudShader(IDirect3DDevice9* device,IDirect3DVertexShader9* original,uint32_t hash);
RECT hudScissor(const RECT& original,const D3DVIEWPORT9& viewport,const Matrix& correction);
}
