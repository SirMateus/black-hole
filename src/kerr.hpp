#pragma once
// Kerr spacetime helpers for the CPU side, in double precision.
// The compute shader (shaders/blackhole.comp) follows every light ray; this file
// does the jobs that only happen once per frame: where the horizons and the ISCO
// are, which observer is holding the camera, and that observer's local axes.
//
// Units: G = c = 1 and M = 0.5, so the Schwarzschild radius Rs = 2M = 1.
//
// Coordinates are Kerr-Schild (x, y, z, t), the same ones the shader uses:
//   * y is up and the accretion disk lies in the plane y = 0,
//   * the hole spins so that the angle phi = atan2(z, x) grows in the prograde
//     direction (in these right-handed axes the spin vector points along -y).
//
// The metric has a famously simple form: flat space plus one extra term,
//   g_mn = eta_mn + f l_m l_n,   eta = diag(+1, +1, +1, -1) in (x, y, z, t) order.
// l is a null vector (a light direction) and f says how strong gravity is here.
// f = 1 at the ergosurface, the place where static observers stop being possible.
//
// 4-vectors are glm::dvec4(x, y, z, t), so v.w is the time component.
#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

namespace kerr {

constexpr double M = 0.5;

// The user-facing spin is the dimensionless a/M in [0, 0.998].
// The length "a" that appears in the metric is spin * M, so at most 0.499.
inline double lengthA(double spin) { return spin * M; }

// Outer (event) and inner (Cauchy) horizons.
inline double rPlus(double a)  { return M + std::sqrt(M * M - a * a); }
inline double rMinus(double a) { return M - std::sqrt(M * M - a * a); }

// Innermost stable circular orbit for gas orbiting with the spin
// (Bardeen, Press & Teukolsky 1972). 3 Rs at spin 0, 0.618 Rs at spin 0.998.
inline double iscoRadius(double spin) {
    double z1 = 1.0 + std::cbrt(1.0 - spin * spin) * (std::cbrt(1.0 + spin) + std::cbrt(1.0 - spin));
    double z2 = std::sqrt(3.0 * spin * spin + z1 * z1);
    return M * (3.0 + z2 - std::sqrt((3.0 - z1) * (3.0 + z1 + 2.0 * z2)));
}

// The radial coordinate r. Surfaces of constant r are squashed spheres (spheroids):
//   (x^2 + z^2) / (r^2 + a^2) + y^2 / r^2 = 1
// and this is the positive solution of that equation.
inline double radius(const glm::dvec3& p, double a) {
    double w = glm::dot(p, p) - a * a;
    return std::sqrt(0.5 * (w + std::sqrt(w * w + 4.0 * a * a * p.y * p.y)));
}

// Everything the metric needs at one point. Valid for r > 0.
struct Local {
    double     r, f;
    glm::dvec3 l;      // spatial part of l_m; its time part is 1
    glm::dvec3 gradR;  // gradient of r
};

inline Local localAt(const glm::dvec3& p, double a) {
    Local g;
    double a2 = a * a, y2 = p.y * p.y;
    g.r = radius(p, a);
    double r2 = g.r * g.r, r3 = r2 * g.r;
    double S = r2 * r2 + a2 * y2, A = r2 + a2;
    g.f = 2.0 * M * r3 / S;
    g.l = {(g.r * p.x + a * p.z) / A, p.y / g.r, (g.r * p.z - a * p.x) / A};
    g.gradR = {p.x * r3 / S, p.y * g.r * A / S, p.z * r3 / S};
    return g;
}

inline glm::dvec3 spatial(const glm::dvec4& v) { return {v.x, v.y, v.z}; }

// l_m v^m
inline double lDot(const Local& g, const glm::dvec4& v) { return v.w + glm::dot(g.l, spatial(v)); }

// g_mn u^m v^n: the "dot product" of curved spacetime.
inline double metricDot(const Local& g, const glm::dvec4& u, const glm::dvec4& v) {
    return -u.w * v.w + glm::dot(spatial(u), spatial(v)) + g.f * lDot(g, u) * lDot(g, v);
}

// v_m = g_mn v^n: the same vector with its index lowered, as the shader wants it.
inline glm::dvec4 lower(const Local& g, const glm::dvec4& v) {
    double s = g.f * lDot(g, v);
    return {spatial(v) + s * g.l, -v.w + s};
}

// ---------------------------------------------------------------------------
// Observers: who is holding the camera?
// ---------------------------------------------------------------------------
// The same spot looks different to observers moving differently (aberration), so the
// picture is only defined once we say who is looking. A 4-velocity u is that choice.

// A static observer hovers at fixed x, y, z with rockets. Only possible while f < 1:
// inside the ergosphere space itself is dragged around faster than light could resist.
inline glm::dvec4 staticVelocity(const Local& g) { return {0.0, 0.0, 0.0, 1.0 / std::sqrt(1.0 - g.f)}; }

// A "rain" observer falls freely from rest far away, with no angular momentum. This is the
// Kerr version of the Painleve-Gullstrand observer, and Doran's coordinates are built on it.
// With q = sqrt(2 M r), s = sqrt(r^2 + a^2) and Q = q / (s + q), its lowered 4-velocity is
//     u_t = -1,   u_i = -Q d_i r
// Raising the index with the inverse metric gives what is computed here. Nothing in it
// divides by zero at the horizons, so a rain observer simply passes through them, and
// it is the only one of our two observers that can exist inside the hole.
// At a = 0 it falls at dr/dtau = -sqrt(Rs / r), the Newtonian escape speed.
inline glm::dvec4 rainVelocity(const Local& g, double a) {
    double q = std::sqrt(2.0 * M * g.r);
    double s = std::sqrt(g.r * g.r + a * a);
    double Q = q / (s + q);
    return {-Q * g.gradR - g.f * (1.0 - Q) * g.l, 1.0 + g.f * (1.0 - Q)};
}

// 0 below lo, 1 above hi, a smooth S-curve between: the same as GLSL's smoothstep
inline double smoothstep(double lo, double hi, double x) {
    double t = std::clamp((x - lo) / (hi - lo), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// How far we are along the change from static (0) to rain (1). f = 1 is where static observers
// stop existing, so f is the right ruler: the handover is over by f = 0.8, safely outside it.
// flightMix fades the change in and out when switching between orbit and free flight.
inline double rainWeight(const Local& g, double rPlus, double flightMix) {
    if (g.f >= 0.8 || g.r < rPlus) return 1.0;  // never evaluate the static observer in here
    return flightMix * smoothstep(0.05, 0.8, g.f);
}

// Between the two, the observer is a weighted sum of their velocities. The sum of two
// future-pointing timelike vectors is another one, so after normalising this is a valid
// observer: one with engines, gradually giving up hovering and starting to fall.
inline glm::dvec4 observerVelocity(const Local& g, double a, double weight) {
    if (weight >= 1.0) return rainVelocity(g, a);
    if (weight <= 0.0) return staticVelocity(g);
    glm::dvec4 u = (1.0 - weight) * staticVelocity(g) + weight * rainVelocity(g, a);
    return u / std::sqrt(-metricDot(g, u, u));
}

// Tetrad: the four orthonormal directions an observer calls "my time", "my right",
// "my up" and "my forward". Light rays are built from these so that the picture is
// what that observer's camera would really record. Stored with lowered indices.
struct Tetrad {
    glm::dvec4 time, right, up, forward;
};

// Gram-Schmidt in the curved metric. Start from the observer's velocity u, then take the
// camera axes (given as plain directions in x, y, z) and remove from each one the parts
// along the previous axes. It cannot degenerate, because surfaces of constant t are
// spacelike everywhere that r > 0. right/up/forward must be orthonormal in the flat sense.
inline Tetrad buildTetrad(const Local& g, const glm::dvec4& u, const glm::dvec3& right,
                          const glm::dvec3& up, const glm::dvec3& forward) {
    auto unit = [&](glm::dvec4 v) { return v / std::sqrt(metricDot(g, v, v)); };
    // u has length^2 = -1, hence the plus sign on that projection.
    glm::dvec4 ef{forward, 0.0};
    ef = unit(ef + metricDot(g, ef, u) * u);
    glm::dvec4 er{right, 0.0};
    er = unit(er + metricDot(g, er, u) * u - metricDot(g, er, ef) * ef);
    glm::dvec4 eu{up, 0.0};
    eu = unit(eu + metricDot(g, eu, u) * u - metricDot(g, eu, ef) * ef - metricDot(g, eu, er) * er);
    return {lower(g, u), lower(g, er), lower(g, eu), lower(g, ef)};
}

// ---------------------------------------------------------------------------
// Where the camera may go
// ---------------------------------------------------------------------------
// Below the inner horizon r- the ring singularity is in reach, rays can leave the region these
// coordinates cover, and the picture stops being meaningful. So the camera stops just above it.
// At low spin r- is close to 0, and the floor of 0.1 keeps it off the central singularity.
inline double minCameraRadius(double a) { return std::max(0.1, 1.02 * rMinus(a)); }
constexpr double kMaxCameraDistance = 300.0;

// r, theta and phi locate a point the same way at any spin, which plain x, y, z do not.
// On the squashed spheres: x + i z = sqrt(r^2 + a^2) sin(theta) e^(i phi),  y = r cos(theta)
struct Spheroidal {
    double r, sinTheta, cosTheta, phi;
};

inline Spheroidal toSpheroidal(const glm::dvec3& p, double a) {
    Spheroidal s;
    s.r = radius(p, a);
    double sin2 = std::min((p.x * p.x + p.z * p.z) / std::max(s.r * s.r + a * a, 1e-300), 1.0);
    s.sinTheta = std::sqrt(sin2);
    s.cosTheta = std::copysign(std::sqrt(1.0 - sin2), p.y);
    s.phi = std::atan2(p.z, p.x);
    return s;
}

inline glm::dvec3 fromSpheroidal(const Spheroidal& s, double a) {
    double rho = std::sqrt(s.r * s.r + a * a) * s.sinTheta;
    return {rho * std::cos(s.phi), s.r * s.cosTheta, rho * std::sin(s.phi)};
}

// Pushes a position back into the allowed region. Moving outward along a straight line would
// not land on a surface of constant r for a > 0, as those surfaces are squashed.
inline glm::dvec3 keepInRange(glm::dvec3 p, double a) {
    double rMin = minCameraRadius(a);
    Spheroidal s = toSpheroidal(p, a);
    if (s.r < rMin) {
        s.r = rMin;
        p = fromSpheroidal(s, a);
    }
    double dist = glm::length(p);
    if (dist > kMaxCameraDistance) p *= kMaxCameraDistance / dist;
    return p;
}

}  // namespace kerr
