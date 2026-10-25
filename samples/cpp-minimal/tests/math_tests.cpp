// Copyright (c) 2026 Hiroshi Matsuyama
// SPDX-License-Identifier: MIT
//
// Unit tests for the sample's matrix helpers (xr_math.h).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cmath>

#include "../src/xr_math.h"

namespace {

struct Vec4 {
    float x, y, z, w;
};

Vec4 Apply(const Mat4& m, float x, float y, float z) {
    Vec4 r{};
    float* out = &r.x;
    const float v[4] = {x, y, z, 1};
    for (int row = 0; row < 4; ++row) {
        for (int k = 0; k < 4; ++k) out[row] += m.m[k * 4 + row] * v[k];
    }
    return r;
}

// Normalized device coordinates and depth of a point.
Vec4 Project(const Mat4& m, float x, float y, float z) {
    const Vec4 c = Apply(m, x, y, z);
    return {c.x / c.w, c.y / c.w, c.z / c.w, c.w};
}

XrQuaternionf Yaw(float angle) {
    return {0, std::sin(angle / 2), 0, std::cos(angle / 2)};
}

const XrFovf kFov45{-0.785398f, 0.785398f, 0.785398f, -0.785398f};  // 45 degrees to each side.
constexpr float kNear = 0.05f, kFar = 100.0f;

}  // namespace

TEST_CASE("ProjectionFov maps the frustum to D3D clip space") {
    const Mat4 p = ProjectionFov(kFov45, kNear, kFar);
    SUBCASE("near and far planes map to depth 0 and 1") {
        CHECK(Project(p, 0, 0, -kNear).z == doctest::Approx(0.0f));
        CHECK(Project(p, 0, 0, -kFar).z == doctest::Approx(1.0f));
    }
    SUBCASE("the FOV edges map to the NDC edges") {
        CHECK(Project(p, 1, 0, -1).x == doctest::Approx(1.0f));  // tan(45 deg) = 1.
        CHECK(Project(p, 0, -1, -1).y == doctest::Approx(-1.0f));
        CHECK(Project(p, 0.5f, 0.25f, -1).x == doctest::Approx(0.5f));
    }
    SUBCASE("w is the distance along -Z") {
        CHECK(Apply(p, 0, 0, -3).w == doctest::Approx(3.0f));
    }
}

TEST_CASE("ProjectionFov handles asymmetric FOVs") {
    // Wider to the right: the view direction is left of the NDC center.
    const XrFovf fov{-0.3f, 0.9f, 0.5f, -0.5f};
    const Mat4 p = ProjectionFov(fov, kNear, kFar);
    CHECK(Project(p, std::tan(-0.3f), 0, -1).x == doctest::Approx(-1.0f));
    CHECK(Project(p, std::tan(0.9f), 0, -1).x == doctest::Approx(1.0f));
    CHECK(Project(p, 0, 0, -1).x < 0.0f);
}

TEST_CASE("FromPose applies scale, rotation, then translation") {
    const Mat4 m = FromPose({Yaw(3.14159265f / 2), {1, 2, 3}}, 2.0f);
    const Vec4 r = Apply(m, 1, 0, 0);  // +X scaled by 2, then turned to -Z, then moved.
    CHECK(r.x == doctest::Approx(1.0f));
    CHECK(r.y == doctest::Approx(2.0f));
    CHECK(r.z == doctest::Approx(1.0f));
}

TEST_CASE("InvertRigid undoes a pose") {
    const XrQuaternionf q{0.1830127f, 0.3535534f, 0.0473672f, 0.9159756f};
    const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    const Mat4 m = FromPose({{q.x / n, q.y / n, q.z / n, q.w / n}, {0.3f, -0.2f, 1.0f}});
    const Vec4 r = Apply(Multiply(InvertRigid(m), m), 0.4f, -0.7f, 1.3f);
    CHECK(r.x == doctest::Approx(0.4f));
    CHECK(r.y == doctest::Approx(-0.7f));
    CHECK(r.z == doctest::Approx(1.3f));
}

TEST_CASE("Multiply composes right to left") {
    const Mat4 moved = FromPose({{0, 0, 0, 1}, {0, 0, -2}});
    const Mat4 scaled = Scale(2, 3, 4);
    const Vec4 r = Apply(Multiply(moved, scaled), 1, 1, 1);  // Scale first, then move.
    CHECK(r.x == doctest::Approx(2.0f));
    CHECK(r.y == doctest::Approx(3.0f));
    CHECK(r.z == doctest::Approx(2.0f));
}

TEST_CASE("A view looking at the container center sees it in the middle") {
    // The eye 0.5 m in front of the container (+Z), as in container space.
    const Mat4 viewProjection =
        Multiply(ProjectionFov(kFov45, kNear, kFar), InvertRigid(FromPose({{0, 0, 0, 1}, {0, 0, 0.5f}})));
    const Vec4 center = Project(viewProjection, 0, 0, 0);
    CHECK(center.x == doctest::Approx(0.0f));
    CHECK(center.y == doctest::Approx(0.0f));
    CHECK(center.z > 0.0f);
    CHECK(center.z < 1.0f);
    // Turned 90 degrees left, the eye looks along -X: a point at -X is centered and
    // -Z is to the right.
    const Mat4 turned =
        Multiply(ProjectionFov(kFov45, kNear, kFar), InvertRigid(FromPose({Yaw(3.14159265f / 2), {0, 0, 0}})));
    CHECK(Project(turned, -1, 0, 0).x == doctest::Approx(0.0f));
    CHECK(Project(turned, -1, 0, -0.5f).x > 0.0f);
}
