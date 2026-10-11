#include "hud.h"
#include <algorithm>
#include <array>
#include <climits>
#include <iterator>
#include <vector>

namespace acah {
namespace {
DWORD registerType(DWORD token) {
    return ((token&D3DSP_REGTYPE_MASK)>>D3DSP_REGTYPE_SHIFT)|
        ((token&D3DSP_REGTYPE_MASK2)>>D3DSP_REGTYPE_SHIFT2);
}
DWORD reg(DWORD type,UINT index) {
    return 0x80000000u|(type<<D3DSP_REGTYPE_SHIFT&D3DSP_REGTYPE_MASK)|
        (type<<D3DSP_REGTYPE_SHIFT2&D3DSP_REGTYPE_MASK2)|index;
}
bool arithmetic(DWORD opcode) {
    return opcode==D3DSIO_MOV || opcode==D3DSIO_ADD || opcode==D3DSIO_MAD ||
        opcode==D3DSIO_MUL || opcode==D3DSIO_RCP;
}
}
bool knownHudShader(uint32_t hash) {
    // These families were disassembled from this executable's captured shaders.
    // Postprocessing, debug figures, and arbitrary WVP shaders are excluded.
    static constexpr uint32_t hashes[]={0xf0e5af1b,0x53dbb633,0xb4d23dfb,0x515bf3d8,
        0x137bbc11,0x25d93280,0x9e108f83,0x08ebd2d0,0x3f66ba19,0x56a73f96,
        0x5bea4698,0x6f970847,0xa41dd422,0xc10a81eb,0xd0e914cc};
    return std::find(std::begin(hashes),std::end(hashes),hash)!=std::end(hashes);
}
bool screenSpaceTransform(const Matrix& transform) {
    // The same primitive family can draw 3D. Only its affine, flat UI use is
    // eligible; a perspective transform must keep the ordinary world path.
    if(!finite(transform) || std::abs(transform.m[15])<1e-6f)return false;
    float epsilon=1e-5f*std::abs(transform.m[15]);
    return std::abs(transform.m[3])<epsilon && std::abs(transform.m[7])<epsilon &&
        std::abs(transform.m[11])<epsilon && std::abs(transform.m[2])<epsilon && std::abs(transform.m[6])<epsilon;
}
bool hudTextureSamplers(IDirect3DPixelShader9* shader,uint32_t& mask) {
    mask=0;
    if(!shader)return false;
    UINT bytes=0;if(FAILED(shader->GetFunction(nullptr,&bytes)) || bytes%4 || bytes<8)return false;
    std::vector<DWORD> code(bytes/4);
    if(FAILED(shader->GetFunction(code.data(),&bytes)) ||
        (code[0]!=D3DPS_VERSION(3,0) && code[0]!=D3DPS_VERSION(2,0)))return false;
    for(size_t p=1;p<code.size();) {
        DWORD opcode=code[p]&D3DSI_OPCODE_MASK;
        if(opcode==D3DSIO_END)return p+1==code.size();
        UINT count=opcode==D3DSIO_COMMENT?((code[p]>>16)&0x7fff):((code[p]&D3DSI_INSTLENGTH_MASK)>>D3DSI_INSTLENGTH_SHIFT);
        if(p+1+count>code.size())return false;
        if(opcode==D3DSIO_DCL) {
            if(count!=2)return false;
            DWORD token=code[p+2];
            if(registerType(token)==D3DSPR_SAMPLER) {
                UINT index=token&D3DSP_REGNUM_MASK;if(index>=16)return false;
                mask|=1u<<index;
            }
        }
        p+=1+count;
    }
    return false;
}
Ptr<IDirect3DVertexShader9> createHudShader(IDirect3DDevice9* device,IDirect3DVertexShader9* original,uint32_t hash) {
    Ptr<IDirect3DVertexShader9> shader;
    UINT bytes=0;if(FAILED(original->GetFunction(nullptr,&bytes)) || bytes%4 || bytes<8)return shader;
    std::vector<DWORD> code(bytes/4);
    if(FAILED(original->GetFunction(code.data(),&bytes)) || code[0]!=D3DVS_VERSION(3,0))return shader;
    UINT position=UINT_MAX,mask=0;std::array<bool,32> temps{};
    bool end=false;
    for(size_t p=1;p<code.size();) {
        DWORD instruction=code[p],opcode=instruction&D3DSI_OPCODE_MASK;
        if(opcode==D3DSIO_END) {end=p+1==code.size();break;}
        UINT count=opcode==D3DSIO_COMMENT?((instruction>>16)&0x7fff):((instruction&D3DSI_INSTLENGTH_MASK)>>D3DSI_INSTLENGTH_SHIFT);
        if(p+1+count>code.size() || !count)return shader;
        if(opcode!=D3DSIO_COMMENT) {
            if(instruction&(D3DSHADER_INSTRUCTION_PREDICATED|D3DSI_COISSUE))return shader;
            if(opcode==D3DSIO_DCL) {
                if(count!=2)return shader;
                DWORD target=code[p+2];
                if(registerType(target)==D3DSPR_OUTPUT && (code[p+1]&D3DSP_DCL_USAGE_MASK)==D3DDECLUSAGE_POSITION) {
                    if(position!=UINT_MAX)return shader;
                    position=target&D3DSP_REGNUM_MASK;
                }
            } else if(opcode==D3DSIO_DEF) {
                if(count!=5 || registerType(code[p+1])!=D3DSPR_CONST || (code[p+1]&D3DSP_REGNUM_MASK)>=kHudConstants)return shader;
            } else {
                if(!arithmetic(opcode))return shader;
                UINT expected=opcode==D3DSIO_MAD?4:(opcode==D3DSIO_MOV || opcode==D3DSIO_RCP?2:3);
                if(count!=expected)return shader;
                for(UINT i=1;i<=count;++i) {
                    DWORD parameter=code[p+i],type=registerType(parameter),index=parameter&D3DSP_REGNUM_MASK;
                    if(parameter&D3DSHADER_ADDRESSMODE_MASK)return shader;
                    if(type==D3DSPR_TEMP) {if(index>=temps.size())return shader;temps[index]=true;}
                    if(type==D3DSPR_CONST && index>=kHudConstants)return shader;
                    if(i==1 && type==D3DSPR_OUTPUT && index==position)mask|=parameter&D3DSP_WRITEMASK_ALL;
                    // VS 3.0 outputs cannot be read; do not reinterpret a shader
                    // that would require rewriting a source/output dependency.
                    if(i>1 && type==D3DSPR_OUTPUT)return shader;
                }
            }
        }
        p+=1+count;
    }
    if(!end || position==UINT_MAX || mask!=D3DSP_WRITEMASK_ALL)return shader;
    auto unused=std::find(temps.begin(),temps.end(),false);if(unused==temps.end())return shader;
    UINT temporary=static_cast<UINT>(unused-temps.begin());temps[temporary]=true;
    auto unusedMask=std::find(temps.begin(),temps.end(),false);if(unusedMask==temps.end())return shader;
    UINT panel=static_cast<UINT>(unusedMask-temps.begin());
    for(size_t p=1;p+1<code.size();) {
        DWORD opcode=code[p]&D3DSI_OPCODE_MASK;
        UINT count=opcode==D3DSIO_COMMENT?((code[p]>>16)&0x7fff):((code[p]&D3DSI_INSTLENGTH_MASK)>>D3DSI_INSTLENGTH_SHIFT);
        if(arithmetic(opcode)) {
            DWORD& destination=code[p+1];
            if(registerType(destination)==D3DSPR_OUTPUT && (destination&D3DSP_REGNUM_MASK)==position)
                destination=(destination&~(D3DSP_REGTYPE_MASK|D3DSP_REGTYPE_MASK2|D3DSP_REGNUM_MASK))|reg(D3DSPR_TEMP,temporary);
        }
        p+=1+count;
    }
    code.pop_back();
    auto dest=[&](UINT index,DWORD mask) {return reg(D3DSPR_TEMP,index)|mask;};
    auto src=[&](UINT index,DWORD swizzle=D3DSP_NOSWIZZLE) {return reg(D3DSPR_TEMP,index)|swizzle;};
    DWORD params=reg(D3DSPR_CONST,kHudConstants+4);
    // The lower-right textured counter panel may remain head-relative. This
    // selection uses stock clip coordinates, before the flight HUD is moved.
    // c252 = {panel left NDC, panel top NDC, enable for textured UI, zero}.
    code.insert(code.end(),{D3DSIO_MUL|(3u<<24),dest(panel,D3DSP_WRITEMASK_0|D3DSP_WRITEMASK_1),
        src(temporary,0xff0000),params|D3DSP_NOSWIZZLE});
    code.insert(code.end(),{D3DSIO_SGE|(3u<<24),dest(panel,D3DSP_WRITEMASK_0),src(temporary,0),src(panel,0)});
    code.insert(code.end(),{D3DSIO_SGE|(3u<<24),dest(panel,D3DSP_WRITEMASK_1),src(panel,0x550000),src(temporary,0x550000)});
    code.insert(code.end(),{D3DSIO_MUL|(3u<<24),dest(panel,D3DSP_WRITEMASK_0),src(panel,0),src(panel,0x550000)});
    code.insert(code.end(),{D3DSIO_MUL|(3u<<24),dest(panel,D3DSP_WRITEMASK_0),src(panel,0),params|0xaa0000});
    code.insert(code.end(),{D3DSIO_IFC|(2u<<24)|(D3DSPC_GT<<16),src(panel,0),params|0xff0000});
    code.insert(code.end(),{D3DSIO_MOV|(2u<<24),reg(D3DSPR_OUTPUT,position)|D3DSP_WRITEMASK_ALL,src(temporary)});
    code.push_back(D3DSIO_ELSE);
    for(UINT component=0;component<4;++component) {
        code.push_back(D3DSIO_DP4|(3u<<D3DSI_INSTLENGTH_SHIFT));
        code.push_back(reg(D3DSPR_OUTPUT,position)|(1u<<(16+component)));
        code.push_back(reg(D3DSPR_TEMP,temporary)|D3DSP_NOSWIZZLE);
        code.push_back(reg(D3DSPR_CONST,kHudConstants+component)|D3DSP_NOSWIZZLE);
    }
    code.push_back(D3DSIO_ENDIF);
    code.push_back(D3DSIO_END);
    HRESULT hr=device->CreateVertexShader(code.data(),shader.GetAddressOf());
    log("spatial HUD shader=%08x POSITION=o%u scratch=r%u/r%u constants=c%u..c%u hr=%08x",hash,position,temporary,panel,kHudConstants,kHudConstants+kHudConstantCount-1,hr);
    return shader;
}
RECT hudScissor(const RECT& original,const D3DVIEWPORT9& viewport,const Matrix& correction) {
    RECT full{static_cast<LONG>(viewport.X),static_cast<LONG>(viewport.Y),
        static_cast<LONG>(viewport.X+viewport.Width),static_cast<LONG>(viewport.Y+viewport.Height)};
    // A full-screen scissor describes the display boundary, not a cropped HUD
    // widget; the eye's display boundary must remain full-screen.
    if(original.left<=full.left && original.top<=full.top && original.right>=full.right && original.bottom>=full.bottom)return full;
    float left=float(full.right),right=float(full.left),top=float(full.bottom),bottom=float(full.top);
    for(int corner=0;corner<4;++corner) {
        float x=2*(float(corner&1?original.right:original.left)-viewport.X)/viewport.Width-1;
        float y=1-2*(float(corner&2?original.bottom:original.top)-viewport.Y)/viewport.Height;
        auto point=DirectX::XMVector4Transform(DirectX::XMVectorSet(x,y,0,1),load(correction));
        DirectX::XMFLOAT4 clip;DirectX::XMStoreFloat4(&clip,point);
        if(!std::isfinite(clip.w) || clip.w<=1e-5f)return full;
        float px=viewport.X+(clip.x/clip.w+1)*0.5f*viewport.Width;
        float py=viewport.Y+(1-clip.y/clip.w)*0.5f*viewport.Height;
        if(!std::isfinite(px) || !std::isfinite(py))return full;
        left=std::min(left,px);right=std::max(right,px);top=std::min(top,py);bottom=std::max(bottom,py);
    }
    return {static_cast<LONG>(std::floor(std::clamp(left,float(full.left),float(full.right)))),
        static_cast<LONG>(std::floor(std::clamp(top,float(full.top),float(full.bottom)))),
        static_cast<LONG>(std::ceil(std::clamp(right,float(full.left),float(full.right)))),
        static_cast<LONG>(std::ceil(std::clamp(bottom,float(full.top),float(full.bottom))))};
}
}
