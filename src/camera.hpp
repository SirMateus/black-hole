#pragma once
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>

#include "kerr.hpp"

// Orbit camera that always looks at the black hole (at the origin).
// Units: 1 = one Schwarzschild radius (Rs).
struct OrbitCamera {
    float yaw      = 0.0f;   // radians, around the vertical axis
    float pitch    = 0.10f;  // radians, above (+) / below (-) the disk plane
    float distance = 22.0f;  // in Rs
    float fovDeg   = 60.0f;  // vertical field of view

    glm::vec3 position() const {
        return distance * glm::vec3(std::cos(pitch) * std::sin(yaw),
                                    std::sin(pitch),
                                    std::cos(pitch) * std::cos(yaw));
    }

    // Orthonormal basis used to build one ray per pixel.
    void basis(glm::vec3& right, glm::vec3& up, glm::vec3& forward) const {
        forward = glm::normalize(-position());
        right   = glm::normalize(glm::cross(forward, glm::vec3(0, 1, 0)));
        up      = glm::cross(right, forward);
    }

    void rotate(float dYaw, float dPitch) {
        yaw  += dYaw;
        pitch = std::clamp(pitch + dPitch, -1.50f, 1.50f);
    }

    void zoom(float factor) {
        distance = std::clamp(distance * factor, 2.5f, 300.0f);
    }

    // Move to the orbit closest to a point. Leaving free flight keeps you where you were, unless
    // you were closer than the orbit camera's limit of distance 2.5: then it moves you out to that.
    void moveTo(const glm::dvec3& p) {
        distance = std::clamp(float(glm::length(p)), 2.5f, 300.0f);
        yaw      = float(std::atan2(p.x, p.z));
        pitch    = std::clamp(float(std::asin(p.y / glm::length(p))), -1.50f, 1.50f);
    }
};

// Free-flight camera. Unlike a real ship, you steer the position directly, even out of
// the horizon. The picture at each point is what the local physical observer sees there.
struct FlyCamera {
    glm::dvec3 pos{0.0, 0.0, 22.0};
    float yaw    = 0.0f;   // radians, around the vertical axis
    float pitch  = 0.0f;   // radians, kept short of straight up or down
    float fovDeg = 60.0f;  // vertical field of view

    void basis(glm::vec3& right, glm::vec3& up, glm::vec3& forward) const {
        forward = {std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw)};
        right   = glm::normalize(glm::cross(forward, glm::vec3(0, 1, 0)));
        up      = glm::cross(right, forward);
    }

    // Take over from the orbit camera, keeping the exact same view.
    void copyFrom(const OrbitCamera& orbit) {
        glm::vec3 right, up, forward;
        orbit.basis(right, up, forward);
        pos    = glm::dvec3(orbit.position());
        yaw    = std::atan2(forward.x, forward.z);
        pitch  = std::asin(forward.y);
        fovDeg = orbit.fovDeg;
    }

    void look(float dYaw, float dPitch) {
        yaw  += dYaw;
        pitch = std::clamp(pitch + dPitch, -1.55f, 1.55f);
    }

    void zoomFov(float factor) {
        fovDeg = std::clamp(fovDeg * factor, 20.0f, 150.0f);
    }

    // input: x = right, y = up, z = forward, each -1, 0 or +1. a is the spin as a length.
    // Speed follows r, with a floor, so the horizon is reached instead of only approached.
    void fly(const glm::vec3& input, bool fast, float dt, double a) {
        glm::dvec3 before = pos;
        if (input != glm::vec3(0.0f)) {
            glm::vec3 right, up, forward;
            basis(right, up, forward);
            glm::dvec3 dir = glm::normalize(glm::dvec3(input.x * right + input.y * up + input.z * forward));
            double speed = 0.5 * std::max(kerr::radius(pos, a), 0.3) * (fast ? 4.0 : 1.0);
            glm::dvec3 next = pos + dir * speed * double(dt);
            // Stop at the floor. Pushing back up would keep the sideways part of the move,
            // and holding W would then carry you round the floor and out the far side.
            if (kerr::radius(next, a) >= kerr::minCameraRadius(a)) pos = next;
        }
        pos = kerr::keepInRange(pos, a);
        if (!std::isfinite(pos.x + pos.y + pos.z)) pos = before;  // a bad value here would stick
    }

    // Stay at the same r, theta, phi when the spin changes, not the same x, y, z:
    // that keeps your place relative to the horizon, which also moves.
    void changeSpin(double oldA, double newA) {
        pos = kerr::keepInRange(kerr::fromSpheroidal(kerr::toSpheroidal(pos, oldA), newA), newA);
    }
};
