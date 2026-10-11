#pragma once
#include "support.h"

namespace acah {
constexpr UINT kTreeConstants=248,kTreeConstantCount=5,kTreeProjection=108;
bool knownTreeShader(uint32_t hash);
Ptr<IDirect3DVertexShader9> createTreeShader(IDirect3DDevice9* device,IDirect3DVertexShader9* original,uint32_t hash);
}
