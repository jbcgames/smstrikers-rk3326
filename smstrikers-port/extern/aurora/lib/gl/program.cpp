#include "program.hpp"

#include "../internal.hpp"
#include "binary_cache.hpp"
#include "census.hpp"

#include <fmt/format.h>

#include <string>
#include <vector>

namespace {
constexpr aurora::gl::GLuint kInvalidBlockIndex = 0xFFFFFFFFu; // GL_INVALID_INDEX
} // namespace

namespace aurora::gl {
namespace {
Module Log("aurora::gl");

GLuint compile_stage(GLenum stage, const char* source, const char* label) {
  const GLuint shader = gl.CreateShader(stage);
  if (shader == 0) {
    Log.error("compile_program({}): glCreateShader failed", label);
    return 0;
  }
  const GLchar* sources[]{source};
  gl.ShaderSource(shader, 1, sources, nullptr);
  gl.CompileShader(shader);

  GLint status = GL_FALSE;
  gl.GetShaderiv(shader, GL_COMPILE_STATUS, &status);
  if (status != GL_TRUE) {
    GLint logLength = 0;
    gl.GetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
    std::string infoLog(logLength > 0 ? static_cast<size_t>(logLength) : 1, '\0');
    gl.GetShaderInfoLog(shader, static_cast<GLsizei>(infoLog.size()), nullptr, infoLog.data());
    Log.error("compile_program({}): {} shader compile failed: {}", label,
              stage == GL_VERTEX_SHADER ? "vertex" : "fragment", infoLog.c_str());
    gl.DeleteShader(shader);
    return 0;
  }
  return shader;
}
} // namespace

GLuint compile_program(const char* vertexSource, const char* fragmentSource, const char* label) {
  const uint64_t cacheKey = binary_cache_key(vertexSource, fragmentSource);

  // 1) Persistent program-binary cache. Try to link from a stored binary on a throwaway program
  //    object; a rejected binary is discarded and we fall through to a clean source compile (no
  //    reuse-after-failed-glProgramBinary hazard). Disabled cache -> straight to source.
  if (binary_cache_enabled()) {
    const GLuint cached = gl.CreateProgram();
    if (cached != 0) {
      if (binary_cache_try_load(cacheKey, cached)) {
        census::programs.add(0);
        return cached;
      }
      gl.DeleteProgram(cached);
    }
  }

  // 2) Compile + link from source.
  const GLuint vs = compile_stage(GL_VERTEX_SHADER, vertexSource, label);
  if (vs == 0) {
    return 0;
  }
  const GLuint fs = compile_stage(GL_FRAGMENT_SHADER, fragmentSource, label);
  if (fs == 0) {
    gl.DeleteShader(vs);
    return 0;
  }

  const GLuint program = gl.CreateProgram();
  gl.AttachShader(program, vs);
  gl.AttachShader(program, fs);
  if (binary_cache_enabled() && gl.ProgramParameteri != nullptr) {
    // Some drivers only keep the binary retrievable when this is set before linking.
    gl.ProgramParameteri(program, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
  }
  gl.LinkProgram(program);
  // The shader objects are reference-counted by the program; drop our references
  // regardless of link result.
  gl.DeleteShader(vs);
  gl.DeleteShader(fs);

  GLint status = GL_FALSE;
  gl.GetProgramiv(program, GL_LINK_STATUS, &status);
  if (status != GL_TRUE) {
    GLint logLength = 0;
    gl.GetProgramiv(program, GL_INFO_LOG_LENGTH, &logLength);
    std::string infoLog(logLength > 0 ? static_cast<size_t>(logLength) : 1, '\0');
    gl.GetProgramInfoLog(program, static_cast<GLsizei>(infoLog.size()), nullptr, infoLog.data());
    Log.error("compile_program({}): link failed: {}", label, infoLog.c_str());
    gl.DeleteProgram(program);
    return 0;
  }

  // 3) Persist the freshly linked binary for the next boot.
  binary_cache_store(cacheKey, program);
  census::programs.add(0);
  return program;
}

void configure_gx_program(GLuint program, uint32_t expectedUniformSize) {
  // GX uniform block -> GL binding point 0. GLSL ES 3.00 has no layout(binding=) on
  // uniform blocks, so we set it after link.
  const GLuint blockIndex = gl.GetUniformBlockIndex(program, "Uniform");
  if (blockIndex != kInvalidBlockIndex) {
    gl.UniformBlockBinding(program, blockIndex, 0);
    // std140 sanity: the driver's block size (16-aligned) must fit inside the CPU
    // uniform range we allocate (expectedUniformSize is that range, aligned up to the
    // UBO offset alignment). A block LARGER than the range means a real layout bug
    // (mis-declared field) and would read past the range at draw time.
    GLint blockSize = 0;
    gl.GetActiveUniformBlockiv(program, blockIndex, GL_UNIFORM_BLOCK_DATA_SIZE, &blockSize);
    if (blockSize > 0 && static_cast<uint32_t>(blockSize) > expectedUniformSize) {
      Log.warn("configure_gx_program: GL uniform block size {} exceeds CPU uniform range {} (std140 layout bug)",
               blockSize, expectedUniformSize);
    }
  }
  // texN sampler uniforms -> texture unit N. glUniform1i needs the program current;
  // this runs on the compiler thread's share context (not the render worker), so it
  // does not disturb the render worker's state cache.
  gl.UseProgram(program);
  for (uint32_t i = 0; i < 8; ++i) {
    const auto name = fmt::format("tex{}", i);
    const GLint loc = gl.GetUniformLocation(program, name.c_str());
    if (loc >= 0) {
      gl.Uniform1i(loc, static_cast<GLint>(i));
    }
  }
  gl.UseProgram(0);
  // Make the linked program + its uniform setup visible to the render worker's context.
  gl.Flush();
}

} // namespace aurora::gl
