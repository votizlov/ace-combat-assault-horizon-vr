#pragma once
#include "vr_math.h"
#include <algorithm>
#include <array>

namespace acah {
// The profiled engine packs four normalized side planes as matrix columns.
// Recover its camera basis from those planes instead of racing the engine's
// mutable global view matrix. The result includes BOTH the native frustum and
// both eye frusta: desktop visibility and native HUD target generation remain.
inline bool visibilityUnion(const Tracking& tracking,const Matrix& p,const Matrix& native,
    float units,Matrix& output,bool& boxFallback) {
    if(!finite(native) || !perspective(p) || std::abs(p.m[8])+std::abs(p.m[9])>1e-5f)return false;
    float h=p.m[11],ax=p.m[0]/std::sqrt(1+p.m[0]*p.m[0]),bx=1/std::sqrt(1+p.m[0]*p.m[0]);
    float ay=p.m[5]/std::sqrt(1+p.m[5]*p.m[5]),by=1/std::sqrt(1+p.m[5]*p.m[5]);
    std::array<float,4> right{},up{},back{},otherBack{};
    for(unsigned row=0;row<4;++row) {
        right[row]=(native.m[row*4]-native.m[row*4+1])/(2*ax);
        up[row]=(native.m[row*4+2]-native.m[row*4+3])/(2*ay);
        back[row]=(native.m[row*4]+native.m[row*4+1])/(2*h*bx);
        otherBack[row]=(native.m[row*4+2]+native.m[row*4+3])/(2*h*by);
    }
    auto dot=[](const auto& a,const auto& b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];};
    if(std::abs(dot(right,right)-1)>0.025f || std::abs(dot(up,up)-1)>0.025f ||
        std::abs(dot(back,back)-1)>0.025f || std::abs(dot(right,up))>0.025f ||
        std::abs(dot(right,back))>0.025f || std::abs(dot(up,back))>0.025f)return false;
    for(unsigned i=0;i<3;++i)if(std::abs(back[i]-otherBack[i])>0.025f)return false;
    if(std::abs(back[3]-otherBack[3])>0.01f+0.001f*std::abs(back[3]))return false;
    using Vector=std::array<float,3>;
    auto cross=[](const Vector& a,const Vector& b){return Vector{a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};};
    auto normalize=[&](Vector& value) {
        float square=dot(value,value);if(!std::isfinite(square) || square<1e-10f)return false;
        float scale=1/std::sqrt(square);for(auto& component:value)component*=scale;return true;
    };
    std::array<Vector,12> rays{};unsigned rayCount=0;
    for(float x:{-1/p.m[0],1/p.m[0]})for(float y:{-1/p.m[5],1/p.m[5]})rays[rayCount++]={x,y,h};
    float largestTangent=1;
    Matrix relative[2];boxFallback=false;
    for(int eye=0;eye<2;++eye) {
        Matrix inverseRelative,eyeProjection;
        if(!eyeMatrices(tracking,eye,p,units,inverseRelative,eyeProjection) || !invert(inverseRelative,relative[eye]))return false;
        const auto& f=tracking.fovs[eye];
        for(float x:{std::tan(f.left),std::tan(f.right)})for(float y:{std::tan(f.down),std::tan(f.up)}) {
            largestTangent=std::max({largestTangent,std::abs(x),std::abs(y)});
            DirectX::XMFLOAT3 ray;
            DirectX::XMStoreFloat3(&ray,DirectX::XMVector3TransformNormal(DirectX::XMVectorSet(x,y,h,0),load(relative[eye])));
            rays[rayCount++]={ray.x,ray.y,ray.z};
        }
    }
    for(auto& ray:rays)if(!normalize(ray))return false;
    // Fit a rotated enclosing cone, rather than a cone forced to point along
    // the aircraft nose. The latter becomes a huge box on ordinary 40–60 degree
    // head turns. A smallest spherical cap is supported by one, two or three
    // corner rays; enumerate those candidates once per cached pose/camera.
    Vector axis{};float best=-2;
    auto consider=[&](Vector candidate) {
        if(!normalize(candidate))return;
        float score=2;for(const auto& ray:rays)score=std::min(score,dot(candidate,ray));
        if(score>best) {best=score;axis=candidate;}
    };
    Vector sum{};for(const auto& ray:rays)for(unsigned i=0;i<3;++i)sum[i]+=ray[i];consider(sum);
    for(unsigned i=0;i<rays.size();++i) {
        consider(rays[i]);
        for(unsigned j=i+1;j<rays.size();++j) {
            Vector pair{},difference{};
            for(unsigned lane=0;lane<3;++lane) {pair[lane]=rays[i][lane]+rays[j][lane];difference[lane]=rays[i][lane]-rays[j][lane];}
            consider(pair);
            for(unsigned k=j+1;k<rays.size();++k) {
                Vector other{};for(unsigned lane=0;lane<3;++lane)other[lane]=rays[i][lane]-rays[k][lane];
                Vector normal=cross(difference,other);consider(normal);for(auto& lane:normal)lane=-lane;consider(normal);
            }
        }
    }
    Vector coneBack{h*axis[0],h*axis[1],h*axis[2]};
    Vector coneRight{1-coneBack[0]*coneBack[0],-coneBack[0]*coneBack[1],-coneBack[0]*coneBack[2]};
    if(!normalize(coneRight)) {coneRight=cross(Vector{0,1,0},coneBack);if(!normalize(coneRight))return false;}
    Vector coneUp=cross(coneBack,coneRight);
    boxFallback=best<=0.02f;
    float left=0,rightSlope=0,down=0,top=0;
    if(!boxFallback)for(const auto& ray:rays) {
        float forward=h*dot(ray,coneBack);
        if(forward<=0.02f) {boxFallback=true;break;}
        float x=dot(ray,coneRight)/forward,y=dot(ray,coneUp)/forward;
        left=std::min(left,x);rightSlope=std::max(rightSlope,x);down=std::min(down,y);top=std::max(top,y);
    }
    // CPU visibility can run a little before the draw's tracking snapshot.
    // Five degrees of angular slack also reduces edge popping during fast turns.
    constexpr float slack=5*DirectX::XM_PI/180;
    float angles[4]={std::atan(left)-slack,std::atan(rightSlope)+slack,std::atan(down)-slack,std::atan(top)+slack};
    for(float angle:angles)if(std::abs(angle)>DirectX::XM_PIDIV2-0.02f)boxFallback=true;
    if(!boxFallback) {left=std::tan(angles[0]);rightSlope=std::tan(angles[1]);down=std::tan(angles[2]);top=std::tan(angles[3]);}
    float normals[4][3]={{1,0,-h*left},{-1,0,h*rightSlope},{0,1,-h*down},{0,-1,h*top}};
    // Only a union too wide for a single forward cone needs the box. Keep its
    // normals nonzero: the engine normalizes again in object coordinates.
    float depthZero=std::abs(p.m[10])>1e-8f?std::abs(p.m[14]/p.m[10]):1000000.0f;
    float depthOne=std::abs(p.m[10]-h)>1e-8f?std::abs(p.m[14]/(p.m[10]-h)):1000000.0f;
    float farDistance=std::max(depthZero,depthOne);
    if(!std::isfinite(farDistance) || farDistance<1)return false;
    float extent=farDistance*4*(1+largestTangent+1/p.m[0]+1/p.m[5]);
    if(boxFallback)for(auto& normal:normals)normal[2]=0;
    else for(auto& normal:normals) {
        Vector changed{};
        for(unsigned lane=0;lane<3;++lane)changed[lane]=normal[0]*coneRight[lane]+normal[1]*coneUp[lane]+normal[2]*coneBack[lane];
        std::copy(changed.begin(),changed.end(),normal);
    }
    output={};
    for(unsigned plane=0;plane<4;++plane) {
        auto& n=normals[plane];float offset=0;
        for(const auto& eye:relative) {
            float position=n[0]*eye.m[12]+n[1]*eye.m[13]+n[2]*eye.m[14];
            offset=std::max(offset,boxFallback?std::abs(position):-position);
        }
        if(boxFallback)offset+=extent;
        offset+=0.02f*units;float length=std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
        for(unsigned row=0;row<4;++row)
            output.m[row*4+plane]=(n[0]*right[row]+n[1]*up[row]+n[2]*back[row]+(row==3?offset:0))/length;
    }
    return finite(output);
}
}
