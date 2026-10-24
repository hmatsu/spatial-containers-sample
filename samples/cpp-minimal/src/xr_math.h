// Copyright (c) 2026 Hiroshi Matsuyama
// SPDX-License-Identifier: MIT
//
// Minimal column-major matrix helpers for rendering OpenXR views.

#pragma once

#include <openxr/openxr.h>

#include <cmath>

struct Mat4 {
    float m[16];  // Column-major: m[column * 4 + row].
};

inline Mat4 Multiply(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int c = 0; c < 4; ++c) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) sum += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = sum;
        }
    }
    return r;
}

// Rotation and translation of a pose, then a uniform scale.
inline Mat4 FromPose(const XrPosef& pose, float scale = 1.0f) {
    const XrQuaternionf& q = pose.orientation;
    const float x2 = q.x + q.x, y2 = q.y + q.y, z2 = q.z + q.z;
    const float xx = q.x * x2, yy = q.y * y2, zz = q.z * z2;
    const float xy = q.x * y2, xz = q.x * z2, yz = q.y * z2;
    const float wx = q.w * x2, wy = q.w * y2, wz = q.w * z2;
    Mat4 r{};
    r.m[0] = (1.0f - yy - zz) * scale;
    r.m[1] = (xy + wz) * scale;
    r.m[2] = (xz - wy) * scale;
    r.m[4] = (xy - wz) * scale;
    r.m[5] = (1.0f - xx - zz) * scale;
    r.m[6] = (yz + wx) * scale;
    r.m[8] = (xz + wy) * scale;
    r.m[9] = (yz - wx) * scale;
    r.m[10] = (1.0f - xx - yy) * scale;
    r.m[12] = pose.position.x;
    r.m[13] = pose.position.y;
    r.m[14] = pose.position.z;
    r.m[15] = 1.0f;
    return r;
}

// Inverse of a rigid transform (rotation + translation only).
inline Mat4 InvertRigid(const Mat4& a) {
    Mat4 r{};
    for (int c = 0; c < 3; ++c) {
        for (int row = 0; row < 3; ++row) r.m[c * 4 + row] = a.m[row * 4 + c];
    }
    for (int row = 0; row < 3; ++row) {
        r.m[12 + row] = -(r.m[row] * a.m[12] + r.m[4 + row] * a.m[13] + r.m[8 + row] * a.m[14]);
    }
    r.m[15] = 1.0f;
    return r;
}

inline Mat4 Scale(float x, float y, float z) {
    Mat4 r{};
    r.m[0] = x;
    r.m[5] = y;
    r.m[10] = z;
    r.m[15] = 1.0f;
    return r;
}

// Projection for an asymmetric OpenXR field of view, with clip space depth in [0, 1]
// (Direct3D convention) and a right-handed view space looking down -Z.
inline Mat4 ProjectionFov(const XrFovf& fov, float nearZ, float farZ) {
    const float left = std::tan(fov.angleLeft), right = std::tan(fov.angleRight);
    const float down = std::tan(fov.angleDown), up = std::tan(fov.angleUp);
    const float width = right - left, height = up - down;
    Mat4 r{};
    r.m[0] = 2.0f / width;
    r.m[5] = 2.0f / height;
    r.m[8] = (right + left) / width;
    r.m[9] = (up + down) / height;
    r.m[10] = -farZ / (farZ - nearZ);
    r.m[11] = -1.0f;
    r.m[14] = -(farZ * nearZ) / (farZ - nearZ);
    return r;
}
