#include "shader.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>

static bool readFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "[shader] cannot open %s\n", path.c_str());
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

static GLuint compileStage(GLenum type, const std::string& path) {
    std::string src;
    if (!readFile(path, src)) return 0;

    GLuint s = glCreateShader(type);
    const char* c = src.c_str();
    glShaderSource(s, 1, &c, nullptr);
    glCompileShader(s);

    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        std::fprintf(stderr, "[shader] compile error in %s:\n%s\n", path.c_str(), log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

GLuint Shader::build() const {
    std::vector<GLuint> shaders;
    for (auto& [type, path] : m_stages) {
        GLuint s = compileStage(type, path);
        if (!s) {
            for (GLuint x : shaders) glDeleteShader(x);
            return 0;
        }
        shaders.push_back(s);
    }

    GLuint p = glCreateProgram();
    for (GLuint s : shaders) glAttachShader(p, s);
    glLinkProgram(p);
    for (GLuint s : shaders) glDeleteShader(s);

    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        std::fprintf(stderr, "[shader] link error:\n%s\n", log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

bool Shader::loadCompute(const std::string& compPath) {
    m_stages = {{GL_COMPUTE_SHADER, compPath}};
    return reload();
}

bool Shader::loadGraphics(const std::string& vertPath, const std::string& fragPath) {
    m_stages = {{GL_VERTEX_SHADER, vertPath}, {GL_FRAGMENT_SHADER, fragPath}};
    return reload();
}

bool Shader::reload() {
    GLuint p = build();
    if (!p) return false;
    if (m_program) glDeleteProgram(m_program);
    m_program = p;
    return true;
}

Shader::~Shader() {
    if (m_program) glDeleteProgram(m_program);
}

void Shader::set(const char* n, int v) const { glUniform1i(glGetUniformLocation(m_program, n), v); }
void Shader::set(const char* n, float v) const { glUniform1f(glGetUniformLocation(m_program, n), v); }
void Shader::set(const char* n, const glm::vec2& v) const { glUniform2f(glGetUniformLocation(m_program, n), v.x, v.y); }
void Shader::set(const char* n, const glm::vec3& v) const { glUniform3f(glGetUniformLocation(m_program, n), v.x, v.y, v.z); }
void Shader::set(const char* n, const glm::vec4& v) const { glUniform4f(glGetUniformLocation(m_program, n), v.x, v.y, v.z, v.w); }
