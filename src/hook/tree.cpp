#include "tree.h"
#include <algorithm>
#include <array>
#include <climits>
#include <vector>

namespace acah {
namespace {
DWORD type(DWORD token) {
    return ((token&D3DSP_REGTYPE_MASK)>>D3DSP_REGTYPE_SHIFT)|
        ((token&D3DSP_REGTYPE_MASK2)>>D3DSP_REGTYPE_SHIFT2);
}
DWORD reg(DWORD kind,UINT index) {
    return 0x80000000u|(kind<<D3DSP_REGTYPE_SHIFT&D3DSP_REGTYPE_MASK)|
        (kind<<D3DSP_REGTYPE_SHIFT2&D3DSP_REGTYPE_MASK2)|index;
}
}
bool knownTreeShader(uint32_t hash) {return hash==0xcc9f4aac || hash==0xfb7f8562;}
Ptr<IDirect3DVertexShader9> createTreeShader(IDirect3DDevice9* device,IDirect3DVertexShader9* original,uint32_t hash) {
    Ptr<IDirect3DVertexShader9> shader;
    if(!knownTreeShader(hash))return shader;
    UINT bytes=0;if(FAILED(original->GetFunction(nullptr,&bytes)) || bytes%4 || bytes<8)return shader;
    std::vector<DWORD> code(bytes/4);
    if(FAILED(original->GetFunction(code.data(),&bytes)) || code[0]!=D3DVS_VERSION(3,0))return shader;
    UINT position=UINT_MAX,mask=0,anchorRegister=UINT_MAX;
    size_t anchorEnd=0;std::array<bool,32> used{};bool end=false;
    for(size_t p=1;p<code.size();) {
        DWORD instruction=code[p],opcode=instruction&D3DSI_OPCODE_MASK;
        if(opcode==D3DSIO_END) {end=p+1==code.size();break;}
        UINT count=opcode==D3DSIO_COMMENT?((instruction>>16)&0x7fff):((instruction&D3DSI_INSTLENGTH_MASK)>>D3DSI_INSTLENGTH_SHIFT);
        if(p+1+count>code.size())return shader;
        if(opcode!=D3DSIO_COMMENT) {
            if(instruction&(D3DSHADER_INSTRUCTION_PREDICATED|D3DSI_COISSUE) || opcode==D3DSIO_RET)return shader;
            if(opcode==D3DSIO_DCL) {
                if(count!=2)return shader;
                DWORD target=code[p+2];
                if(type(target)==D3DSPR_OUTPUT && (code[p+1]&D3DSP_DCL_USAGE_MASK)==D3DDECLUSAGE_POSITION) {
                    if(position!=UINT_MAX)return shader;
                    position=target&D3DSP_REGNUM_MASK;
                }
            } else if(opcode==D3DSIO_DEF || opcode==D3DSIO_DEFI || opcode==D3DSIO_DEFB) {
                if(!count)return shader;
                if(type(code[p+1])==D3DSPR_CONST && (code[p+1]&D3DSP_REGNUM_MASK)>=kTreeConstants)return shader;
            } else {
                for(UINT i=1;i<=count;++i) {
                    DWORD parameter=code[p+i],kind=type(parameter),index=parameter&D3DSP_REGNUM_MASK;
                    if(kind==D3DSPR_TEMP) {if(index>=used.size())return shader;used[index]=true;}
                    if(kind==D3DSPR_CONST && index>=kTreeConstants)return shader;
                    if(kind==D3DSPR_OUTPUT && index==position) {
                        if(i!=1 || (parameter&D3DSHADER_ADDRESSMODE_MASK))return shader;
                        mask|=parameter&D3DSP_WRITEMASK_ALL;
                    }
                }
                // Both captured tree shaders finish their unbiased center with
                // MAD rN, view.w, c111, rN. Preserve it before fog reuses rN.z.
                if(opcode==D3DSIO_MAD && count==4 && type(code[p+1])==D3DSPR_TEMP &&
                    (code[p+1]&D3DSP_WRITEMASK_ALL)==D3DSP_WRITEMASK_ALL &&
                    type(code[p+3])==D3DSPR_CONST && (code[p+3]&D3DSP_REGNUM_MASK)==kTreeProjection+3) {
                    if(anchorEnd)return shader;
                    anchorRegister=code[p+1]&D3DSP_REGNUM_MASK;anchorEnd=p+1+count;
                }
            }
        }
        p+=1+count;
    }
    if(!end || position==UINT_MAX || mask!=D3DSP_WRITEMASK_ALL || !anchorEnd)return shader;
    std::array<UINT,3> scratch{};
    for(auto& index:scratch) {
        auto free=std::find(used.begin(),used.end(),false);if(free==used.end())return shader;
        index=static_cast<UINT>(free-used.begin());used[index]=true;
    }
    UINT output=scratch[0],anchor=scratch[1],result=scratch[2];
    std::vector<DWORD> patched{code[0]};
    for(size_t p=1;p+1<code.size();) {
        DWORD opcode=code[p]&D3DSI_OPCODE_MASK;
        UINT count=opcode==D3DSIO_COMMENT?((code[p]>>16)&0x7fff):((code[p]&D3DSI_INSTLENGTH_MASK)>>D3DSI_INSTLENGTH_SHIFT);
        size_t start=patched.size();patched.insert(patched.end(),code.begin()+p,code.begin()+p+1+count);
        if(opcode!=D3DSIO_COMMENT && opcode!=D3DSIO_DCL && count) {
            DWORD& destination=patched[start+1];
            if(type(destination)==D3DSPR_OUTPUT && (destination&D3DSP_REGNUM_MASK)==position)
                destination=(destination&~(D3DSP_REGTYPE_MASK|D3DSP_REGTYPE_MASK2|D3DSP_REGNUM_MASK))|reg(D3DSPR_TEMP,output);
        }
        p+=1+count;
        if(p==anchorEnd)patched.insert(patched.end(),{D3DSIO_MOV|(2u<<24),reg(D3DSPR_TEMP,anchor)|D3DSP_WRITEMASK_ALL,
            reg(D3DSPR_TEMP,anchorRegister)|D3DSP_NOSWIZZLE});
    }
    auto src=[&](UINT index,DWORD swizzle=D3DSP_NOSWIZZLE) {return reg(D3DSPR_TEMP,index)|swizzle;};
    auto dest=[&](UINT index,DWORD mask) {return reg(D3DSPR_TEMP,index)|mask;};
    DWORD zero=reg(D3DSPR_CONST,kTreeConstants+4); // Host supplies zero in all lanes.
    // Preserve the game's culled vertices. The DIRECTPOS branch writes z=-1;
    // FIXEDPOS writes w=0. Neither should be unprojected into visible geometry.
    patched.insert(patched.end(),{D3DSIO_IFC|(2u<<24)|(D3DSPC_GT<<16),src(output,0xff0000),zero});
    patched.insert(patched.end(),{D3DSIO_IFC|(2u<<24)|(D3DSPC_GE<<16),src(output,0xaa0000),zero});
    patched.insert(patched.end(),{D3DSIO_IFC|(2u<<24)|(D3DSPC_LE<<16),src(output,0xaa0000),src(output,0xff0000)});
    // Stock output is NDC with w=1. Restore its X/Y using the unbiased center's
    // homogeneous w, not the intentionally biased depth used for tree sorting.
    patched.insert(patched.end(),{D3DSIO_MUL|(3u<<24),dest(anchor,D3DSP_WRITEMASK_0|D3DSP_WRITEMASK_1),src(output),src(anchor,0xff0000)});
    for(UINT component:{0u,1u,3u})patched.insert(patched.end(),{D3DSIO_DP4|(3u<<24),dest(result,1u<<(16+component)),
        src(anchor),reg(D3DSPR_CONST,kTreeConstants+component)|D3DSP_NOSWIZZLE});
    // WVP is still corrected normally for the original per-eye biased Z/W.
    // Rehomogenize that depth against the new w without moving the quad anchor.
    patched.insert(patched.end(),{D3DSIO_MUL|(3u<<24),dest(result,D3DSP_WRITEMASK_2),src(output,0xaa0000),src(result,0xff0000)});
    patched.insert(patched.end(),{D3DSIO_MOV|(2u<<24),reg(D3DSPR_OUTPUT,position)|D3DSP_WRITEMASK_ALL,src(result)});
    for(int branch=0;branch<3;++branch) {
        patched.push_back(D3DSIO_ELSE);
        patched.insert(patched.end(),{D3DSIO_MOV|(2u<<24),reg(D3DSPR_OUTPUT,position)|D3DSP_WRITEMASK_ALL,src(output)});
        patched.push_back(D3DSIO_ENDIF);
    }
    patched.push_back(D3DSIO_END);
    HRESULT hr=device->CreateVertexShader(patched.data(),shader.GetAddressOf());
    log("tree stereo shader=%08x POSITION=o%u scratch=r%u/r%u/r%u center=r%u constants=c%u..c%u hr=%08x",
        hash,position,output,anchor,result,anchorRegister,kTreeConstants,kTreeConstants+kTreeConstantCount-1,hr);
    return shader;
}
}
