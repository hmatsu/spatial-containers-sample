// Copyright (c) 2026 Hiroshi Matsuyama
// SPDX-License-Identifier: MIT
//
// Unit tests for the stateless parts of the emulation layer (layer_internal.h).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cmath>
#include <map>
#include <string>

#include "../src/layer_internal.h"

using namespace scemu;

namespace {

// Settings for ParseConfig, standing in for the environment.
std::map<std::string, std::string> g_settings;

const char* Lookup(const char* name) {
    const auto it = g_settings.find(name);
    return it == g_settings.end() ? nullptr : it->second.c_str();
}

Config Parse(std::map<std::string, std::string> settings) {
    g_settings = std::move(settings);
    return ParseConfig(Lookup);
}

constexpr float kPi = 3.14159265358979f;

// Unit quaternion for a rotation about `axis` (normalized here).
XrQuaternionf AxisAngle(XrVector3f axis, float angle) {
    const float length = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
    const float s = std::sin(angle / 2) / length;
    return {axis.x * s, axis.y * s, axis.z * s, std::cos(angle / 2)};
}

XrVector3f Transform(const XrPosef& pose, const XrVector3f& v) {
    const XrVector3f r = Rotate(pose.orientation, v);
    return {r.x + pose.position.x, r.y + pose.position.y, r.z + pose.position.z};
}

void CheckVector(const XrVector3f& actual, const XrVector3f& expected) {
    CHECK(actual.x == doctest::Approx(expected.x).epsilon(1e-5));
    CHECK(actual.y == doctest::Approx(expected.y).epsilon(1e-5));
    CHECK(actual.z == doctest::Approx(expected.z).epsilon(1e-5));
}

const XrQuaternionf kIdentity{0, 0, 0, 1};
const XrFovf kWideFov{-0.8f, 0.8f, 0.7f, -0.7f};

}  // namespace

TEST_CASE("ParseFloat3 accepts three finite comma-separated numbers") {
    float v[3] = {9, 9, 9};
    CHECK(ParseFloat3("0.3,-0.2,1e-1", v));
    CHECK(v[0] == doctest::Approx(0.3f));
    CHECK(v[1] == doctest::Approx(-0.2f));
    CHECK(v[2] == doctest::Approx(0.1f));

    float untouched[3] = {1, 2, 3};
    CHECK_FALSE(ParseFloat3(nullptr, untouched));
    CHECK_FALSE(ParseFloat3("1,2", untouched));
    CHECK_FALSE(ParseFloat3("a,b,c", untouched));
    CHECK_FALSE(ParseFloat3("1,inf,3", untouched));
    CHECK_FALSE(ParseFloat3("nan,1,1", untouched));
    CHECK(untouched[0] == 1);  // Not written on failure.
}

TEST_CASE("ParseConfig defaults") {
    const Config config = Parse({});
    CHECK(config.bounds.width == 0.5f);
    CHECK_FALSE(config.boundsFromEnv);
    CHECK(config.position.z == -1.0f);
    CHECK(config.maxContainers == 16);
    CHECK(config.supportsBounded);
    CHECK(config.supportsImmersive);
    CHECK_FALSE(config.denyVisibleRequests);
    CHECK_FALSE(config.denyBoundsModeRequests);
    CHECK(config.tightFov);
    CHECK_FALSE(config.log);
}

TEST_CASE("ParseConfig reads every setting") {
    const Config config = Parse({{"XR_SCEMU_BOUNDS", "0.3,0.2,0.25"},
                                 {"XR_SCEMU_POSITION", "0,-0.1,-0.5"},
                                 {"XR_SCEMU_SUPPORTED_MODES", "immersive"},
                                 {"XR_SCEMU_MAX_CONTAINERS", "3"},
                                 {"XR_SCEMU_DENY_VISIBLE_REQUESTS", "1"},
                                 {"XR_SCEMU_DENY_BOUNDS_MODE_REQUESTS", "yes"},
                                 {"XR_SCEMU_TIGHT_FOV", "0"},
                                 {"XR_SCEMU_LOG", "1"}});
    CHECK(config.boundsFromEnv);
    CHECK(config.bounds.depth == doctest::Approx(0.25f));
    CHECK(config.position.y == doctest::Approx(-0.1f));
    CHECK_FALSE(config.supportsBounded);
    CHECK(config.supportsImmersive);
    CHECK(config.maxContainers == 3);
    CHECK(config.denyVisibleRequests);
    CHECK(config.denyBoundsModeRequests);
    CHECK_FALSE(config.tightFov);
    CHECK(config.log);
}

TEST_CASE("ParseConfig rejects invalid values") {
    SUBCASE("non-positive bounds keep the default") {
        const Config config = Parse({{"XR_SCEMU_BOUNDS", "0.3,0,0.2"}});
        CHECK_FALSE(config.boundsFromEnv);
        CHECK(config.bounds.width == 0.5f);
    }
    SUBCASE("no recognized mode means both") {
        const Config config = Parse({{"XR_SCEMU_SUPPORTED_MODES", "none"}});
        CHECK(config.supportsBounded);
        CHECK(config.supportsImmersive);
    }
    SUBCASE("empty and 0 flags are off") {
        const Config config = Parse({{"XR_SCEMU_LOG", ""}, {"XR_SCEMU_DENY_VISIBLE_REQUESTS", "0"}});
        CHECK_FALSE(config.log);
        CHECK_FALSE(config.denyVisibleRequests);
    }
}

TEST_CASE("FindInChain") {
    XrBaseInStructure c{XR_TYPE_SPATIAL_CONTAINER_STATE_EXT, nullptr};
    XrBaseInStructure b{XR_TYPE_SPATIAL_CONTAINER_BOUNDS_EXT, &c};
    CHECK(FindInChain<XrBaseInStructure>(&b, XR_TYPE_SPATIAL_CONTAINER_STATE_EXT) == &c);
    CHECK(FindInChain<XrBaseInStructure>(&b, XR_TYPE_SPATIAL_CONTAINER_BOUNDS_EXT) == &b);
    CHECK(FindInChain<XrBaseInStructure>(&b, XR_TYPE_SESSION_CREATE_INFO) == nullptr);
    CHECK(FindInChain<XrBaseInStructure>(nullptr, XR_TYPE_SESSION_CREATE_INFO) == nullptr);
}

TEST_CASE("ScopedChainUnlink removes structures and restores the chain") {
    XrBaseInStructure d{XR_TYPE_SPATIAL_CONTAINER_STATE_EXT, nullptr};
    XrBaseInStructure c{XR_TYPE_SESSION_CREATE_INFO_SPATIAL_CONTAINERS_EXT, &d};
    XrBaseInStructure b{XR_TYPE_SPATIAL_CONTAINER_BOUNDS_EXT, &c};
    XrBaseInStructure a{XR_TYPE_SESSION_CREATE_INFO_SPATIAL_CONTAINERS_EXT, &b};
    XrBaseInStructure head{XR_TYPE_SESSION_CREATE_INFO, &a};
    {
        // The head itself is never removed; matches anywhere after it are.
        const ScopedChainUnlink unlink(&head, {XR_TYPE_SESSION_CREATE_INFO_SPATIAL_CONTAINERS_EXT});
        CHECK(head.next == &b);
        CHECK(b.next == &d);
        CHECK(d.next == nullptr);
    }
    CHECK(head.next == &a);
    CHECK(a.next == &b);
    CHECK(b.next == &c);
    CHECK(c.next == &d);
}

TEST_CASE("ScopedChainUnlink with several types and adjacent matches") {
    XrBaseInStructure c{XR_TYPE_SPATIAL_CONTAINER_STATE_EXT, nullptr};
    XrBaseInStructure b{XR_TYPE_SPATIAL_CONTAINER_BOUNDS_EXT, &c};
    XrBaseInStructure head{XR_TYPE_FRAME_END_INFO, &b};
    {
        const ScopedChainUnlink unlink(&head, {XR_TYPE_SPATIAL_CONTAINER_BOUNDS_EXT, XR_TYPE_SPATIAL_CONTAINER_STATE_EXT});
        CHECK(head.next == nullptr);
    }
    CHECK(head.next == &b);
    CHECK(b.next == &c);
}

TEST_CASE("CopyLayer copies projection layers") {
    XrBaseInStructure extra{XR_TYPE_SPATIAL_CONTAINER_STATE_EXT, nullptr};
    XrCompositionLayerProjectionView views[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW, &extra},
                                                 {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    views[1].fov.angleRight = 0.5f;
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION, &extra};
    layer.viewCount = 2;
    layer.views = views;
    const auto* header = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);

    SUBCASE("for this frame, next chains are kept") {
        LayerCopy copy;
        REQUIRE(CopyLayer(header, false, &copy));
        CHECK(copy.projection.next == &extra);
        CHECK(copy.projection.views == copy.projectionViews.data());
        CHECK(copy.projection.views != views);
        CHECK(copy.projectionViews[0].next == &extra);
        CHECK(copy.projectionViews[1].fov.angleRight == 0.5f);
    }
    SUBCASE("for retain, next chains are dropped") {
        LayerCopy copy;
        REQUIRE(CopyLayer(header, true, &copy));
        CHECK(copy.projection.next == nullptr);
        CHECK(copy.projectionViews[0].next == nullptr);
    }
    SUBCASE("moving a copy keeps its views pointer valid") {
        std::vector<LayerCopy> copies;
        for (int i = 0; i < 8; ++i) {  // Forces reallocation.
            LayerCopy copy;
            CopyLayer(header, true, &copy);
            copies.push_back(std::move(copy));
        }
        for (const LayerCopy& copy : copies) {
            CHECK(copy.projection.views == copy.projectionViews.data());
            CHECK(copy.Header()->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION);
        }
    }
}

TEST_CASE("CopyLayer copies quads and rejects other layer types") {
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    quad.size = {0.4f, 0.3f};
    LayerCopy copy;
    REQUIRE(CopyLayer(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad), true, &copy));
    CHECK(copy.Header()->type == XR_TYPE_COMPOSITION_LAYER_QUAD);
    CHECK(copy.quad.size.width == 0.4f);

    XrCompositionLayerCubeKHR cube{XR_TYPE_COMPOSITION_LAYER_CUBE_KHR};
    LayerCopy other;
    CHECK_FALSE(CopyLayer(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&cube), true, &other));
}

TEST_CASE("Rotate, Inverse and Multiply") {
    SUBCASE("a quarter turn about +Y maps +X to -Z") {
        CheckVector(Rotate(AxisAngle({0, 1, 0}, kPi / 2), {1, 0, 0}), {0, 0, -1});
    }
    SUBCASE("a quarter turn about +X maps +Y to +Z") {
        CheckVector(Rotate(AxisAngle({1, 0, 0}, kPi / 2), {0, 1, 0}), {0, 0, 1});
    }
    const XrPosef a{AxisAngle({0.267f, 0.535f, 0.802f}, 1.1f), {0.3f, -0.2f, 1.0f}};
    const XrPosef b{AxisAngle({1, 0, 0}, -0.5f), {-1.0f, 0.5f, 0.25f}};
    const XrVector3f v{0.4f, -0.7f, 1.3f};
    SUBCASE("Multiply applies b first, then a") {
        CheckVector(Transform(Multiply(a, b), v), Transform(a, Transform(b, v)));
    }
    SUBCASE("Inverse undoes a pose") {
        CheckVector(Transform(Inverse(a), Transform(a, v)), v);
        CheckVector(Transform(Multiply(a, Inverse(a)), v), v);
    }
}

TEST_CASE("FitFov narrows the FOV to a box straight ahead") {
    // A 0.2 m cube, 1 m ahead (its front face at 0.9 m).
    const XrPosef containerInView{kIdentity, {0, 0, -1}};
    XrFovf fov = kWideFov;
    float scale[2] = {};
    REQUIRE(FitFov(containerInView, {0.2f, 0.2f, 0.2f}, &fov, scale) == FitResult::Fitted);
    const float edge = std::atan(0.1f / 0.9f);
    CHECK(fov.angleLeft == doctest::Approx(-edge));
    CHECK(fov.angleRight == doctest::Approx(edge));
    CHECK(fov.angleUp == doctest::Approx(edge));
    CHECK(fov.angleDown == doctest::Approx(-edge));
    CHECK(scale[0] == doctest::Approx((0.2f / 0.9f) / (2 * std::tan(0.8f))));
    CHECK(scale[1] == doctest::Approx((0.2f / 0.9f) / (2 * std::tan(0.7f))));
}

TEST_CASE("FitFov is asymmetric for an off-center box") {
    const XrPosef containerInView{kIdentity, {0.3f, 0, -1}};
    XrFovf fov = kWideFov;
    float scale[2] = {};
    REQUIRE(FitFov(containerInView, {0.2f, 0.2f, 0.2f}, &fov, scale) == FitResult::Fitted);
    CHECK(fov.angleLeft == doctest::Approx(std::atan(0.2f / 1.1f)));  // Near-left edge is the far face.
    CHECK(fov.angleRight == doctest::Approx(std::atan(0.4f / 0.9f)));
    CHECK(fov.angleLeft > 0);  // Entirely to the right of the view direction.
}

TEST_CASE("FitFov accounts for the container's rotation") {
    // A thin 0.4 m x 0.1 m x 0.02 m panel, 1 m ahead, turned 90 degrees about +Y:
    // its long side now points along the view direction, so it looks narrow.
    const XrPosef turned{AxisAngle({0, 1, 0}, kPi / 2), {0, 0, -1}};
    XrFovf fov = kWideFov;
    float scale[2] = {};
    REQUIRE(FitFov(turned, {0.4f, 0.1f, 0.02f}, &fov, scale) == FitResult::Fitted);
    CHECK(fov.angleRight == doctest::Approx(std::atan(0.01f / 0.8f)));
    CHECK(fov.angleUp == doctest::Approx(std::atan(0.05f / 0.8f)));
}

TEST_CASE("FitFov clamps to the original FOV") {
    // A box much wider than the view: the FOV stays the original one.
    const XrPosef containerInView{kIdentity, {0, 0, -1}};
    XrFovf fov = kWideFov;
    float scale[2] = {};
    REQUIRE(FitFov(containerInView, {10.0f, 0.2f, 0.2f}, &fov, scale) == FitResult::Fitted);
    CHECK(fov.angleLeft == doctest::Approx(kWideFov.angleLeft));
    CHECK(fov.angleRight == doctest::Approx(kWideFov.angleRight));
    CHECK(scale[0] == doctest::Approx(1.0f));
}

TEST_CASE("FitFov leaves the FOV alone when the box reaches the eye") {
    XrFovf fov = kWideFov;
    float scale[2] = {1, 1};
    SUBCASE("eye inside the box") {
        CHECK(FitFov({kIdentity, {0, 0, 0}}, {0.5f, 0.5f, 0.5f}, &fov, scale) == FitResult::Unchanged);
    }
    SUBCASE("box behind the eye") {
        CHECK(FitFov({kIdentity, {0, 0, 1}}, {0.2f, 0.2f, 0.2f}, &fov, scale) == FitResult::Unchanged);
    }
    SUBCASE("front face closer than 1 cm") {
        // The front face is 5 mm ahead: projecting it would blow up the angles.
        CHECK(FitFov({kIdentity, {0, 0, -0.105f}}, {0.2f, 0.2f, 0.2f}, &fov, scale) == FitResult::Unchanged);
    }
    CHECK(fov.angleRight == kWideFov.angleRight);
}

TEST_CASE("FitFov reports a box outside the view") {
    // 3 m to the right at 1 m: tan = 2.9 / 1.1 at the nearest edge, beyond tan(0.8).
    XrFovf fov = kWideFov;
    float scale[2] = {};
    CHECK(FitFov({kIdentity, {3, 0, -1}}, {0.2f, 0.2f, 0.2f}, &fov, scale) == FitResult::Outside);
    CHECK(fov.angleRight == kWideFov.angleRight);
}
