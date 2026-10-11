#pragma once
#include "protocol.h"
#include <DirectXMath.h>
#include <cmath>
#include <cstring>

namespace acah {
struct Matrix { float m[16]; };
static_assert(sizeof(Matrix)==64);
inline DirectX::XMMATRIX load(const Matrix& m) {
    return DirectX::XMLoadFloat4x4(reinterpret_cast<const DirectX::XMFLOAT4X4*>(m.m));
}
inline Matrix store(DirectX::FXMMATRIX m) {
    Matrix out{}; DirectX::XMStoreFloat4x4(reinterpret_cast<DirectX::XMFLOAT4X4*>(out.m), m); return out;
}
inline Matrix identity() { return store(DirectX::XMMatrixIdentity()); }
inline Matrix transpose(const Matrix& m) { return store(DirectX::XMMatrixTranspose(load(m))); }
inline Matrix multiply(const Matrix& a, const Matrix& b) { return store(load(a) * load(b)); }
inline bool invert(const Matrix& m, Matrix& result) {
    DirectX::XMVECTOR determinant;
    auto inverse = DirectX::XMMatrixInverse(&determinant, load(m));
    float det = DirectX::XMVectorGetX(determinant);
    if (!std::isfinite(det) || std::abs(det) < 1e-12f) return false;
    result = store(inverse); return true;
}
inline bool finite(const Matrix& m) { for (float f : m.m) if (!std::isfinite(f)) return false; return true; }
inline bool perspective(const Matrix& m) {
    return finite(m) && m.m[0] > 0.05f && m.m[0] < 50 && m.m[5] > 0.05f && m.m[5] < 50 &&
        std::abs(m.m[15]) < 1e-5f && std::abs(std::abs(m.m[11]) - 1) < 1e-4f && std::abs(m.m[14]) > 1e-7f &&
        std::abs(m.m[1]) < 1e-5f && std::abs(m.m[4]) < 1e-5f && std::abs(m.m[3]) < 1e-5f && std::abs(m.m[7]) < 1e-5f;
}
inline Matrix poseMatrix(const Pose& p) {
    auto q = DirectX::XMVectorSet(p.orientation[0],p.orientation[1],p.orientation[2],p.orientation[3]);
    return store(DirectX::XMMatrixRotationQuaternion(DirectX::XMQuaternionNormalize(q)) *
        DirectX::XMMatrixTranslation(p.position[0],p.position[1],p.position[2]));
}
inline bool eyeMatrices(const Tracking& t, int eye, const Matrix& originalProjection, float units,
    Matrix& inverseRelative, Matrix& projection) {
    if (!t.valid || eye < 0 || eye > 1 || !perspective(originalProjection)) return false;
    Matrix inverseOrigin;
    if (!invert(poseMatrix(t.origin), inverseOrigin)) return false;
    // Row vectors: relative eye transform = eye-local-to-XR * XR-to-recentered.
    Matrix relative = multiply(poseMatrix(t.eyes[eye]), inverseOrigin);
    // OpenXR and ACAH use right-handed camera coordinates. Convert only for
    // a left-handed source projection; the game's reversed depth is preserved.
    const float handedness=originalProjection.m[11] < 0 ? -1.0f : 1.0f;
    if (handedness > 0) {
        Matrix flip = identity(); flip.m[10] = -1;
        relative = multiply(multiply(flip, relative), flip);
    }
    relative.m[12] *= units; relative.m[13] *= units; relative.m[14] *= units;
    if (!invert(relative, inverseRelative)) return false;
    const auto& f = t.fovs[eye];
    float l=std::tan(f.left), r=std::tan(f.right), u=std::tan(f.up), d=std::tan(f.down);
    if (!(r > l && u > d)) return false;
    // Keep the game's depth terms/near-far range, replace the off-axis X/Y frustum.
    projection={};
    projection.m[0]=2/(r-l); projection.m[5]=2/(u-d);
    projection.m[8]=-handedness*(r+l)/(r-l); projection.m[9]=-handedness*(u+d)/(u-d);
    projection.m[10]=originalProjection.m[10]; projection.m[11]=handedness; projection.m[14]=originalProjection.m[14];
    return finite(inverseRelative) && finite(projection);
}
inline bool correction(const Tracking& t, int eye, const Matrix& originalProjection, float units, Matrix& result) {
    Matrix inverseOriginal,inverseRelative,projection;
    if (!invert(originalProjection,inverseOriginal) ||
        !eyeMatrices(t,eye,originalProjection,units,inverseRelative,projection)) return false;
    result=multiply(multiply(inverseOriginal,inverseRelative),projection);
    return finite(result);
}
inline bool hudCorrection(const Tracking& t,int eye,const Matrix& originalProjection,float units,float depthMeters,Matrix& result) {
    Matrix inverseRelative,projection;
    if(!std::isfinite(depthMeters) || depthMeters<0 || units<=0 ||
        !eyeMatrices(t,eye,originalProjection,units,inverseRelative,projection))return false;
    const float handedness=originalProjection.m[11]<0?-1.0f:1.0f;
    // Convert the stock UI's homogeneous clip X/Y/W to a camera-space ray.
    // Ignore its Z: this is a UI sorting layer, not the target's world depth.
    Matrix ray{};
    ray.m[0]=1/originalProjection.m[0];ray.m[5]=1/originalProjection.m[5];
    ray.m[12]=-handedness*originalProjection.m[8]/originalProjection.m[0];
    ray.m[13]=-handedness*originalProjection.m[9]/originalProjection.m[5];
    ray.m[14]=handedness;
    // Zero means a collimated HUD at infinity; head translation then cancels.
    // A finite plane uses the same game-unit conversion as the eye view.
    ray.m[15]=depthMeters>0?1/(depthMeters*units):0;
    result=multiply(multiply(ray,inverseRelative),projection);
    // HUD depth testing is disabled. Keep Z inside the clip volume even for
    // reversed/infinite projections, while preserving X/Y/W and behind-eye clipping.
    for(int row=0;row<4;++row)result.m[row*4+2]=0.5f*result.m[row*4+3];
    return finite(result);
}
}
