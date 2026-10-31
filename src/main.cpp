// Real-time Kerr (spinning) black hole
// ------------------------------------
// Every pixel shoots a light ray backwards from the camera. A compute shader
// bends each ray through curved spacetime (shaders/blackhole.comp), checks
// whether it falls into the hole, crosses the accretion disk or the spacetime
// grid, or escapes to the starfield. A second pass adds bloom + tone mapping.
//
// Units: Schwarzschild radius Rs = 2M = 1. For a hole with no spin the event
// horizon is at r = 1, the photon sphere at r = 1.5 and the innermost stable
// orbit at r = 3. For gas and light going round with the spin, all three move
// inward as the spin grows.

#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "camera.hpp"
#include "kerr.hpp"
#include "shader.hpp"

namespace fs = std::filesystem;

#ifdef _WIN32
// On laptops with two GPUs, ask for the fast one
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

// ---------------------------------------------------------------------------
// All tunables in one place
// ---------------------------------------------------------------------------
struct Settings {
    bool  showDisk   = true;
    bool  showGrid   = false;
    bool  doppler    = true;   // relativistic beaming + redshift colouring
    bool  bloom      = true;
    bool  autoOrbit  = false;
    bool  paused     = false;  // freezes the disk rotation
    float resScale   = 1.0f;   // render resolution relative to the window
    int   maxSteps   = 1024;   // max integration steps per ray
    float stepTol    = 0.05f;  // integrator tolerance (smaller = more accurate, slower)
    float spin       = 0.9f;   // a/M, from 0 to 0.998
    float diskOuter  = 14.0f;  // the inner edge follows the spin: it is the ISCO
    float exposure   = 1.0f;
    float brightGoal = 1.0f;   // auto-exposure puts the bright end of the lit image at this level
    float minExpose  = 0.08f;  // and never stops down further than this
    float insideFov  = 150.0f; // free flight widens to this field of view deep inside the horizon
};

// Spin values visited by K and L. A table keeps a = 0 and 0.998 exact.
static const float kSpinSteps[] = {0.0f, 0.1f, 0.3f, 0.5f, 0.7f, 0.9f, 0.95f, 0.99f, 0.998f};

struct App {
    GLFWwindow* window = nullptr;
    OrbitCamera cam;
    FlyCamera   fly;
    bool        flying = false;
    float       flightMix = 0.0f;  // fades the free-flight observer in and out
    Settings    s;
    Shader      tracer, screen;
    GLuint      texture = 0, vao = 0;
    int         texW = 0, texH = 0, texLevels = 0;
    float       autoExposure = 1.0f, frameBrightness = 0.0f;
    bool        dragging = false;
    double      lastX = 0, lastY = 0;
    float       simTime = 0.0f;
    double      fps = 0.0;
    double      camR = 0.0, observerWeight = 0.0;  // for the title bar
    bool        camInside = false;
    bool        titleStale = false;  // the view changed: refresh the title after the next frame
};

static void printHelp() {
    std::printf(
        "\nControls\n"
        "  F                 switch between orbit camera and free flight\n"
        "  K / L             spin down / up\n"
        "  T                 toggle accretion disk\n"
        "  G                 toggle spacetime grid (Flamm's paraboloid)\n"
        "  P                 toggle Doppler beaming / redshift\n"
        "  B                 toggle bloom\n"
        "  Space             pause disk rotation\n"
        "  + / -             render resolution up / down (for slower GPUs)\n"
        "  [ / ]             exposure down / up\n"
        "  1 / 2 / 3         orbit camera presets (side / above / far), also leave free flight\n"
        "  R                 reload shaders from disk\n"
        "  H                 print this help\n"
        "  Esc               leave free flight, or quit\n"
        "\nOrbit camera\n"
        "  Left mouse drag   orbit\n"
        "  Mouse wheel       zoom\n"
        "  O                 toggle auto-orbit\n"
        "\nFree flight\n"
        "  W A S D           fly forward / left / back / right\n"
        "  Q / E             down / up\n"
        "  Shift             faster (speed also grows with distance from the hole)\n"
        "  Mouse             look around\n"
        "  Mouse wheel       field of view (it also widens by itself below the horizon)\n\n");
}

// The render texture has mipmaps so the screen pass can use them as a cheap bloom.
static void ensureTexture(App& a, int winW, int winH) {
    int w = std::max(16, int(winW * a.s.resScale));
    int h = std::max(16, int(winH * a.s.resScale));
    if (w == a.texW && h == a.texH && a.texture) return;

    if (a.texture) glDeleteTextures(1, &a.texture);
    glGenTextures(1, &a.texture);
    glBindTexture(GL_TEXTURE_2D, a.texture);
    int levels = 1 + int(std::floor(std::log2(double(std::max(w, h)))));
    glTexStorage2D(GL_TEXTURE_2D, levels, GL_RGBA16F, w, h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    a.texW = w;
    a.texH = h;
    a.texLevels = levels;
}

// Auto-exposure, like a camera stopping down in bright light. The reading is the 90th percentile
// of the brightness of the lit part of the image. The peak would be a poor reading: one small
// blinding spot would darken everything else, turn the gas grey and take the stars with it.
// Exposure stays between minExpose and 1, so the a = 0 views keep the original look.
// adapt is the fraction of the way to go each frame, from 0 to 1.
static void updateAutoExposure(App& a, float adapt) {
    // A level 130 to 250 texels wide. The very small mip levels are no good here: once halving
    // reaches an odd size their averages become uneven, and on some drivers the right edge of
    // the image goes missing from them altogether.
    int level = std::max(0, a.texLevels - 8);
    int w = std::max(1, a.texW >> level), h = std::max(1, a.texH >> level);
    static std::vector<float> rgba, lit;
    rgba.resize(size_t(w) * h * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, a.texture);
    glGetTexImage(GL_TEXTURE_2D, level, GL_RGBA, GL_FLOAT, rgba.data());

    // Undo the exposure already in the image, so that the reading depends only on the scene.
    // The threshold leaves out the sky: a view of nothing but stars has no glare to meter.
    lit.clear();
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
        float luminance = (0.2126f * rgba[i] + 0.7152f * rgba[i + 1] + 0.0722f * rgba[i + 2]) / a.autoExposure;
        if (luminance > 0.1f) lit.push_back(luminance);
    }
    a.frameBrightness = 0.0f;
    if (!lit.empty()) {
        auto percentile = lit.begin() + std::ptrdiff_t(lit.size() * 9 / 10);
        std::nth_element(lit.begin(), percentile, lit.end());
        a.frameBrightness = *percentile;
    }
    // A small bright object can sit near the top of the tone curve. Gas filling the frame needs
    // to come down to the middle of it, or its texture flattens out.
    float litShare = float(lit.size()) / float(size_t(w) * h);
    float goal = a.s.brightGoal * glm::mix(1.0f, 0.4f, float(kerr::smoothstep(0.35, 0.8, litShare)));
    float wanted = a.frameBrightness > 0.0f ? goal / a.frameBrightness : 1.0f;
    wanted = std::clamp(wanted, a.s.minExpose, 1.0f);
    a.autoExposure *= std::pow(wanted / a.autoExposure, adapt);
}

static void renderFrame(App& a, float adapt) {
    int winW, winH;
    glfwGetFramebufferSize(a.window, &winW, &winH);
    if (winW == 0 || winH == 0) return;  // minimised
    ensureTexture(a, winW, winH);

    // --- Pass 1: ray-trace curved spacetime in the compute shader ----------
    glm::vec3 right, up, fwd;
    glm::dvec3 pos;
    float fovDeg;
    if (a.flying) {
        a.fly.basis(right, up, fwd);
        pos = a.fly.pos;
        fovDeg = a.fly.fovDeg;
    } else {
        a.cam.basis(right, up, fwd);
        pos = glm::dvec3(a.cam.position());
        fovDeg = a.cam.fovDeg;
    }

    // Who is holding the camera, and what are their local axes? The picture is built from
    // these. The orbit camera stays far enough out to be held by a static observer. In free
    // flight the observer changes from static to free-falling on the way in (see kerr.hpp).
    double spinA = kerr::lengthA(a.s.spin);
    double rPlus = kerr::rPlus(spinA);
    kerr::Local local = kerr::localAt(pos, spinA);
    a.camR = local.r;
    a.camInside = local.r < rPlus;
    a.observerWeight = kerr::rainWeight(local, rPlus, a.flightMix);
    // Flying straight in, the edge of the sky soon moves past the corners of a normal view, and
    // at low spin the whole window would go black (below r = 0.63 at a = 0). So the lens widens
    // with depth below the horizon, which keeps the black ahead ringed by the sky.
    if (a.flying) {
        float depth = float(kerr::smoothstep(0.0, 0.9, rPlus - local.r));
        fovDeg = glm::mix(fovDeg, std::max(fovDeg, a.s.insideFov), depth);
    }
    kerr::Tetrad frame = kerr::buildTetrad(local, kerr::observerVelocity(local, spinA, a.observerWeight),
                                           right, up, fwd);

    a.tracer.use();
    a.tracer.set("uCamPos", glm::vec3(pos));
    a.tracer.set("uFrameTime", glm::vec4(frame.time));
    a.tracer.set("uFrameRight", glm::vec4(frame.right));
    a.tracer.set("uFrameUp", glm::vec4(frame.up));
    a.tracer.set("uFrameForward", glm::vec4(frame.forward));
    a.tracer.set("uCamInside", int(a.camInside));
    a.tracer.set("uTanHalfFov", std::tan(glm::radians(fovDeg) * 0.5f));
    a.tracer.set("uTime", a.simTime);
    a.tracer.set("uShowDisk", int(a.s.showDisk));
    a.tracer.set("uShowGrid", int(a.s.showGrid));
    a.tracer.set("uDoppler", int(a.s.doppler));
    a.tracer.set("uMaxSteps", a.s.maxSteps);
    // The costliest views are a fast-spinning hole seen from nearby, where rays wind around it
    // many times. A coarser tolerance there saves about a third of the work.
    double heavyView = kerr::smoothstep(0.9, 0.99, a.s.spin) * (1.0 - kerr::smoothstep(6.0, 12.0, local.r));
    a.tracer.set("uStepTol", float(a.s.stepTol * (1.0 + 0.6 * heavyView)));
    a.tracer.set("uA", float(spinA));
    a.tracer.set("uRPlus", float(rPlus));
    a.tracer.set("uDiskInner", float(kerr::iscoRadius(a.s.spin)));
    a.tracer.set("uDiskOuter", a.s.diskOuter);
    a.tracer.set("uEscapeRadius", std::max(60.0f, float(local.r) * 1.5f));
    a.tracer.set("uAutoExposure", a.autoExposure);

    glBindImageTexture(0, a.texture, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glDispatchCompute((a.texW + 15) / 16, (a.texH + 15) / 16, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT | GL_TEXTURE_UPDATE_BARRIER_BIT);

    glBindTexture(GL_TEXTURE_2D, a.texture);
    glGenerateMipmap(GL_TEXTURE_2D);
    updateAutoExposure(a, adapt);

    // --- Pass 2: bloom + tone mapping onto the window ----------------------
    glViewport(0, 0, winW, winH);
    a.screen.use();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, a.texture);
    a.screen.set("uImage", 0);
    a.screen.set("uBloom", int(a.s.bloom));
    a.screen.set("uExposure", a.s.exposure);
    glBindVertexArray(a.vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);  // one fullscreen triangle
}

static void updateTitle(App& a) {
    const char* observer = a.observerWeight <= 0.0 ? "static"
                         : a.observerWeight >= 1.0 ? "free-falling" : "static -> falling";
    char title[220];
    std::snprintf(title, sizeof(title),
                  "Black Hole  |  a = %.3f  |  %s  r = %.2f%s  |  observer: %s  |  %.0f FPS  |  %dx%d (%d%%)  |  H = help",
                  a.s.spin, a.flying ? "fly" : "orbit", a.camR, a.camInside ? "  INSIDE THE HORIZON" : "",
                  observer, a.fps, a.texW, a.texH, int(a.s.resScale * 100));
    glfwSetWindowTitle(a.window, title);
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
static void captureMouse(App& a, bool on) {
    glfwSetInputMode(a.window, GLFW_CURSOR, on ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    if (glfwRawMouseMotionSupported())
        glfwSetInputMode(a.window, GLFW_RAW_MOUSE_MOTION, on ? GLFW_TRUE : GLFW_FALSE);
    glfwGetCursorPos(a.window, &a.lastX, &a.lastY);  // or the next mouse move would be a big jump
}

static void setFlying(App& a, bool on) {
    if (on == a.flying) return;
    a.flying = on;
    a.dragging = false;
    if (on) a.fly.copyFrom(a.cam);
    else    a.cam.moveTo(a.fly.pos);
    captureMouse(a, on);
    a.titleStale = true;
}

static void setSpin(App& a, float spin) {
    if (a.flying) a.fly.changeSpin(kerr::lengthA(a.s.spin), kerr::lengthA(spin));
    a.s.spin = spin;
    a.titleStale = true;
}

static void setPreset(App& a, float pitch, float distance) {
    setFlying(a, false);
    a.cam.pitch = pitch;
    a.cam.distance = distance;
    a.titleStale = true;
}


static void onKey(GLFWwindow* w, int key, int, int action, int) {
    if (action != GLFW_PRESS && action != GLFW_REPEAT) return;
    App& a = *static_cast<App*>(glfwGetWindowUserPointer(w));
    auto onoff = [](bool b) { return b ? "on" : "off"; };

    // Holding a key repeats it. That suits the sliders below, but not the on/off keys.
    if (action == GLFW_REPEAT) {
        switch (key) {
        case GLFW_KEY_K: case GLFW_KEY_L:
        case GLFW_KEY_EQUAL: case GLFW_KEY_KP_ADD: case GLFW_KEY_MINUS: case GLFW_KEY_KP_SUBTRACT:
        case GLFW_KEY_LEFT_BRACKET: case GLFW_KEY_RIGHT_BRACKET: break;
        default: return;
        }
    }

    switch (key) {
    case GLFW_KEY_ESCAPE:
        if (a.flying) setFlying(a, false);
        else glfwSetWindowShouldClose(w, 1);
        break;
    case GLFW_KEY_F: setFlying(a, !a.flying); break;
    case GLFW_KEY_T: a.s.showDisk = !a.s.showDisk; std::printf("disk %s\n", onoff(a.s.showDisk)); break;
    case GLFW_KEY_G: a.s.showGrid = !a.s.showGrid; std::printf("grid %s\n", onoff(a.s.showGrid)); break;
    case GLFW_KEY_P: a.s.doppler = !a.s.doppler; std::printf("doppler %s\n", onoff(a.s.doppler)); break;
    case GLFW_KEY_B: a.s.bloom = !a.s.bloom; std::printf("bloom %s\n", onoff(a.s.bloom)); break;
    case GLFW_KEY_O: if (!a.flying) a.s.autoOrbit = !a.s.autoOrbit; break;
    case GLFW_KEY_K:
        for (int i = int(std::size(kSpinSteps)) - 1; i >= 0; --i)
            if (kSpinSteps[i] < a.s.spin - 1e-4f) { setSpin(a, kSpinSteps[i]); break; }
        break;
    case GLFW_KEY_L:
        for (float step : kSpinSteps)
            if (step > a.s.spin + 1e-4f) { setSpin(a, step); break; }
        break;
    case GLFW_KEY_SPACE: a.s.paused = !a.s.paused; break;
    case GLFW_KEY_EQUAL: case GLFW_KEY_KP_ADD:
        a.s.resScale = std::min(1.0f, a.s.resScale + 0.125f); break;
    case GLFW_KEY_MINUS: case GLFW_KEY_KP_SUBTRACT:
        a.s.resScale = std::max(0.25f, a.s.resScale - 0.125f); break;
    case GLFW_KEY_LEFT_BRACKET:  a.s.exposure = std::max(a.s.exposure / 1.2f, 0.01f); break;
    case GLFW_KEY_RIGHT_BRACKET: a.s.exposure = std::min(a.s.exposure * 1.2f, 100.0f); break;
    case GLFW_KEY_1: setPreset(a, 0.10f, 22.0f); break;
    case GLFW_KEY_2: setPreset(a, 1.10f, 25.0f); break;
    case GLFW_KEY_3: setPreset(a, 0.25f, 60.0f); break;
    case GLFW_KEY_H: printHelp(); break;
    case GLFW_KEY_R: {
        bool ok = a.tracer.reload() & a.screen.reload();
        std::printf(ok ? "shaders reloaded\n" : "shader reload failed (kept old version)\n");
        break;
    }
    default: break;
    }
}

static void onMouseButton(GLFWwindow* w, int button, int action, int) {
    App& a = *static_cast<App*>(glfwGetWindowUserPointer(w));
    if (button == GLFW_MOUSE_BUTTON_LEFT && !a.flying) {
        a.dragging = (action == GLFW_PRESS);
        glfwGetCursorPos(w, &a.lastX, &a.lastY);
    }
}

static void onCursor(GLFWwindow* w, double x, double y) {
    App& a = *static_cast<App*>(glfwGetWindowUserPointer(w));
    if (a.flying) a.fly.look(float(a.lastX - x) * 0.002f, float(a.lastY - y) * 0.002f);
    else if (a.dragging) a.cam.rotate(float(a.lastX - x) * 0.005f, float(y - a.lastY) * 0.005f);
    a.lastX = x;
    a.lastY = y;
}

static void onScroll(GLFWwindow* w, double, double yoff) {
    App& a = *static_cast<App*>(glfwGetWindowUserPointer(w));
    if (a.flying) a.fly.zoomFov(std::pow(0.9f, float(yoff)));
    else a.cam.zoom(std::pow(0.9f, float(yoff)));
}

static void onFocus(GLFWwindow* w, int focused) {
    App& a = *static_cast<App*>(glfwGetWindowUserPointer(w));
    if (focused) glfwGetCursorPos(w, &a.lastX, &a.lastY);
}

static void updateFlight(App& a, float dt) {
    auto held = [&](int key) { return glfwGetKey(a.window, key) == GLFW_PRESS ? 1.0f : 0.0f; };
    glm::vec3 input(held(GLFW_KEY_D) - held(GLFW_KEY_A),
                    held(GLFW_KEY_E) - held(GLFW_KEY_Q),
                    held(GLFW_KEY_W) - held(GLFW_KEY_S));
    bool fast = held(GLFW_KEY_LEFT_SHIFT) + held(GLFW_KEY_RIGHT_SHIFT) > 0.0f;
    // A long frame (moving the window, reloading shaders) must not turn into a huge jump
    a.fly.fly(input, fast, std::min(dt, 0.05f), kerr::lengthA(a.s.spin));
}

// --record: a fixed flight from the default orbit down through the horizon, for videos.
// u runs from 0 to 1 over the clip. r shrinks at a steady rate, as it does in free flight, and
// the camera drifts round the hole while staying above the disk plane, so it passes through
// the gap in the middle of the disk, not through the gas.
static void recordedPose(FlyCamera& cam, double u, double a) {
    double theta = glm::radians(glm::mix(84.0, 70.0, u)), phi = glm::radians(glm::mix(90.0, 112.0, u));
    kerr::Spheroidal s{22.0 * std::pow(0.33 / 22.0, u), std::sin(theta), std::cos(theta), phi};
    cam.pos = kerr::fromSpheroidal(s, a);
    glm::dvec3 forward = -glm::normalize(cam.pos);
    cam.yaw = float(std::atan2(forward.x, forward.z));
    cam.pitch = float(std::asin(forward.y));
    // Light bent by the spinning hole shifts the shadow sideways near the end, so aim after it
    cam.yaw += float(glm::radians(38.0 * kerr::smoothstep(0.45, 0.85, u)));
}

// ---------------------------------------------------------------------------
static fs::path findShaderDir(const char* argv0) {
    std::vector<fs::path> candidates = {
        fs::absolute(argv0).parent_path() / "shaders",
        fs::current_path() / "shaders",
        fs::absolute(argv0).parent_path().parent_path() / "shaders",
    };
    for (auto& p : candidates)
        if (fs::exists(p / "blackhole.comp")) return p;
    return candidates.front();
}

static void saveScreenshot(GLFWwindow* w, const char* path) {
    int W, H;
    glfwGetFramebufferSize(w, &W, &H);
    std::vector<unsigned char> px(size_t(W) * H * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, W, H, GL_RGB, GL_UNSIGNED_BYTE, px.data());
    std::ofstream f(path, std::ios::binary);
    f << "P6\n" << W << " " << H << "\n255\n";
    for (int y = H - 1; y >= 0; --y) f.write((char*)&px[size_t(y) * W * 3], W * 3);
}

static void printUsage() {
    std::printf(
        "Usage: black_hole [options]\n"
        "  --spin a                start with this spin (a/M, 0 to 0.998)\n"
        "  --view 1|2|3            start from a camera preset\n"
        "  --fly x y z yaw pitch   start in free flight, looking along yaw and pitch (degrees)\n"
        "  --fov deg               vertical field of view\n"
        "  --size W H              window size (default 1280 720)\n"
        "  --grid                  start with the spacetime grid on\n"
        "  --no-doppler            start with Doppler beaming off\n"
        "  --no-disk               start with the disk off\n"
        "  --time t                freeze the disk at this time, for repeatable images\n"
        "  --screenshot out.ppm    render a few frames, save the image, exit\n"
        "  --record folder [s]     fly into the hole for s seconds (default 6) at 30 FPS, saving frames\n"
        "  --bench N               time N frames with vsync off, print the average, exit\n"
        "  --verbose               also print the exposure with --screenshot\n"
        "  --help                  print this\n");
}

int main(int argc, char** argv) {
    const char* screenshotPath = nullptr;
    fs::path recordDir;
    float recordSeconds = 6.0f;
    int startView = 1, benchFrames = 0;
    bool startGrid = false, freezeTime = false, startDoppler = true, startDisk = true, startFly = false, verbose = false;
    float startSpin = Settings{}.spin, startTime = 0.0f, startFov = 0.0f;
    FlyCamera startPose;
    int winW = 1280, winH = 720;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        int values = argc - i - 1;
        if (arg == "--help" || arg == "-h") { printUsage(); return 0; }
        else if (arg == "--screenshot" && values >= 1) screenshotPath = argv[++i];
        else if (arg == "--record" && values >= 1) {
            recordDir = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] != '-') recordSeconds = float(std::atof(argv[++i]));
        }
        else if (arg == "--view" && values >= 1) startView = std::atoi(argv[++i]);
        else if (arg == "--grid") startGrid = true;
        else if (arg == "--spin" && values >= 1) startSpin = float(std::atof(argv[++i]));
        else if (arg == "--fly" && values >= 5) {
            startFly = true;
            startPose.pos = {std::atof(argv[i + 1]), std::atof(argv[i + 2]), std::atof(argv[i + 3])};
            startPose.yaw = glm::radians(float(std::atof(argv[i + 4])));
            startPose.pitch = glm::radians(float(std::atof(argv[i + 5])));
            i += 5;
        }
        else if (arg == "--fov" && values >= 1) startFov = std::clamp(float(std::atof(argv[++i])), 1.0f, 170.0f);
        else if (arg == "--size" && values >= 2) { winW = std::atoi(argv[i + 1]); winH = std::atoi(argv[i + 2]); i += 2; }
        else if (arg == "--time" && values >= 1) { startTime = float(std::atof(argv[++i])); freezeTime = true; }
        else if (arg == "--no-doppler") startDoppler = false;
        else if (arg == "--no-disk") startDisk = false;
        else if (arg == "--bench" && values >= 1) benchFrames = std::atoi(argv[++i]);
        else if (arg == "--verbose") verbose = true;
        else {
            std::fprintf(stderr, "Unknown option or missing value: %s\n\n", argv[i]);
            printUsage();
            return 1;
        }
    }
    if (!std::isfinite(startSpin) || winW < 16 || winH < 16) {
        std::fprintf(stderr, "--spin needs a number and --size at least 16 16\n");
        return 1;
    }
    const float recordFps = 30.0f;
    const int recordWarmup = 45;  // lets the exposure settle before the first saved frame
    int recordFrames = recordDir.empty() ? 0 : std::max(2, int(recordSeconds * recordFps));
    bool offscreen = screenshotPath || benchFrames > 0 || recordFrames > 0;
    if (!glfwInit()) {
        std::fprintf(stderr, "Failed to initialise GLFW\n");
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_VISIBLE, offscreen ? GLFW_FALSE : GLFW_TRUE);
    glfwWindowHint(GLFW_DECORATED, offscreen ? GLFW_FALSE : GLFW_TRUE);  // or a title bar limits the size

    App a;
    a.s.showGrid = startGrid;
    a.s.doppler = startDoppler;
    a.s.showDisk = startDisk;
    a.s.spin = std::clamp(startSpin, 0.0f, 0.998f);
    a.simTime = startTime;
    a.s.paused = freezeTime;
    if (startView == 2) { a.cam.pitch = 1.10f; a.cam.distance = 25.0f; }
    if (startView == 3) { a.cam.pitch = 0.25f; a.cam.distance = 60.0f; }
    if (startFov > 0.0f) { a.cam.fovDeg = startFov; startPose.fovDeg = startFov; }
    if (recordFrames > 0) {
        recordedPose(startPose, 0.0, kerr::lengthA(a.s.spin));
        startFly = true;
    }
    if (startFly) {
        a.fly = startPose;
        a.fly.pos = kerr::keepInRange(a.fly.pos, kerr::lengthA(a.s.spin));
        a.flying = true;
        a.flightMix = 1.0f;
    }
    a.window = glfwCreateWindow(winW, winH, "Black Hole", nullptr, nullptr);
    if (!a.window) {
        std::fprintf(stderr,
            "Could not create an OpenGL 4.3 window.\n"
            "Your GPU or driver is too old for compute shaders - update your graphics driver.\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(a.window);
    glfwSwapInterval(benchFrames > 0 ? 0 : 1);

    int version = gladLoadGL(glfwGetProcAddress);
    if (!version) {
        std::fprintf(stderr, "Failed to load OpenGL functions\n");
        return 1;
    }
    std::printf("OpenGL %d.%d  |  %s\n", GLAD_VERSION_MAJOR(version), GLAD_VERSION_MINOR(version),
                (const char*)glGetString(GL_RENDERER));

    fs::path dir = findShaderDir(argv[0]);
    std::printf("Shaders: %s\n", dir.string().c_str());
    if (!a.tracer.loadCompute((dir / "blackhole.comp").string()) ||
        !a.screen.loadGraphics((dir / "screen.vert").string(), (dir / "screen.frag").string())) {
        std::fprintf(stderr, "Shader setup failed - see errors above.\n");
        return 1;
    }

    glGenVertexArrays(1, &a.vao);  // core profile needs a VAO bound even with no vertex data

    glfwSetWindowUserPointer(a.window, &a);
    glfwSetKeyCallback(a.window, onKey);
    glfwSetMouseButtonCallback(a.window, onMouseButton);
    glfwSetCursorPosCallback(a.window, onCursor);
    glfwSetScrollCallback(a.window, onScroll);
    glfwSetWindowFocusCallback(a.window, onFocus);
    if (a.flying && !offscreen) captureMouse(a, true);
    if (!offscreen) printHelp();
    if (recordFrames > 0) fs::create_directories(recordDir);

    double last = glfwGetTime(), fpsTimer = last;
    int frames = 0, totalFrames = 0;
    double benchSeconds = 0.0;
    const int warmupFrames = 5;  // the first frames also pay for shader start-up

    while (!glfwWindowShouldClose(a.window)) {
        double now = glfwGetTime();
        float dt = recordFrames > 0 ? 1.0f / recordFps : float(now - last);
        last = now;
        if (!a.s.paused) a.simTime += dt;
        if (recordFrames > 0) {
            double u = std::max(totalFrames - recordWarmup, 0) / double(recordFrames - 1);
            recordedPose(a.fly, u, kerr::lengthA(a.s.spin));
        }
        else if (a.flying) updateFlight(a, dt);
        else if (a.s.autoOrbit) a.cam.rotate(dt * 0.15f, 0.0f);
        // Fade the observer over 0.3 s, so pressing F does not make the view jump
        a.flightMix = std::clamp(a.flightMix + (a.flying ? dt : -dt) / 0.3f, 0.0f, 1.0f);

        // With no one watching the eye adapt, settle quickly: images must be repeatable
        bool settleFast = offscreen && recordFrames == 0;
        renderFrame(a, settleFast ? 0.5f : 1.0f - std::exp(-dt / 0.25f));
        if (a.titleStale) {
            updateTitle(a);
            a.titleStale = false;
        }

        if (benchFrames > 0) {
            glFinish();
            if (totalFrames >= warmupFrames) benchSeconds += glfwGetTime() - now;
            if (++totalFrames == warmupFrames + benchFrames) {
                double ms = 1000.0 * benchSeconds / benchFrames;
                std::printf("bench: %dx%d  %.2f ms/frame  %.0f FPS\n", a.texW, a.texH, ms, 1000.0 / ms);
                break;
            }
        }
        if (screenshotPath && ++totalFrames == 40) {
            saveScreenshot(a.window, screenshotPath);
            std::printf("saved %s\n", screenshotPath);
            if (verbose) std::printf("auto exposure %.4f, meter %.4f\n", a.autoExposure, a.frameBrightness);
            break;
        }
        if (recordFrames > 0) {
            if (totalFrames >= recordWarmup) {
                char name[32];
                std::snprintf(name, sizeof(name), "frame_%05d.ppm", totalFrames - recordWarmup);
                saveScreenshot(a.window, (recordDir / name).string().c_str());
            }
            if (++totalFrames == recordWarmup + recordFrames) {
                std::printf("saved %d frames to %s\n", recordFrames, recordDir.string().c_str());
                break;
            }
        }
        glfwSwapBuffers(a.window);
        glfwPollEvents();

        ++frames;
        if (now - fpsTimer > 0.5) {
            a.fps = frames / (now - fpsTimer);
            updateTitle(a);
            frames = 0;
            fpsTimer = now;
        }
    }

    // Free GPU objects while the OpenGL context still exists
    glDeleteTextures(1, &a.texture);
    glDeleteVertexArrays(1, &a.vao);
    a.tracer = {};
    a.screen = {};
    glfwTerminate();
    return 0;
}
