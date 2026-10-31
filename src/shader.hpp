#pragma once
#include <glad/gl.h>
#include <glm/glm.hpp>
#include <string>
#include <utility>
#include <vector>

// Small helper around an OpenGL shader program.
// Shaders are read from disk at runtime, so they can be edited and
// hot-reloaded (press R) without recompiling the C++ code.
class Shader {
public:
    // Compute program: one .comp file.
    bool loadCompute(const std::string& compPath);
    // Graphics program: vertex + fragment files.
    bool loadGraphics(const std::string& vertPath, const std::string& fragPath);

    // Rebuild from the same files. On failure the old program is kept.
    bool reload();

    void use() const { glUseProgram(m_program); }
    GLuint id() const { return m_program; }

    void set(const char* name, int v) const;
    void set(const char* name, float v) const;
    void set(const char* name, const glm::vec2& v) const;
    void set(const char* name, const glm::vec3& v) const;
    void set(const char* name, const glm::vec4& v) const;

    Shader() = default;
    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;
    Shader& operator=(Shader&& o) noexcept {
        std::swap(m_program, o.m_program);
        std::swap(m_stages, o.m_stages);
        return *this;
    }
    ~Shader();

private:
    GLuint build() const;  // returns 0 on error
    std::vector<std::pair<GLenum, std::string>> m_stages;
    GLuint m_program = 0;
};
