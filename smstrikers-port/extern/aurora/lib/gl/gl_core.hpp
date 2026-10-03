#pragma once

// GL ES 3.0 + EGL entry points and enumerants for the hand-rolled backend.
//
// House style (matches lib/webgpu/sdl2shim_present.cpp:33-215): aurora includes
// NO vendor GLES/EGL headers. Every enum is a local constexpr and every entry
// point is a function pointer resolved at load time through the shim's getProc
// chain. This keeps us decoupled from whatever <GLES3/gl3.h> the device ships
// and lets the same binary bind libmali, PowerVR, or desktop Mesa.
//
// The table is filled once per owning thread's context by gl::load() (see
// gl_loader.cpp) and read through the global `aurora::gl::gl`.

#include <cstddef>
#include <cstdint>

namespace aurora::gl {

// ---- Scalar type aliases (ABI-compatible with the vendor headers) ----
using GLenum = uint32_t;
using GLboolean = uint8_t;
using GLbitfield = uint32_t;
using GLbyte = int8_t;
using GLubyte = uint8_t;
using GLshort = int16_t;
using GLushort = uint16_t;
using GLint = int32_t;
using GLuint = uint32_t;
using GLsizei = int32_t;
using GLfloat = float;
using GLclampf = float;
using GLchar = char;
using GLintptr = intptr_t;
using GLsizeiptr = intptr_t;
using GLint64 = int64_t;
using GLuint64 = uint64_t;
using GLsync = void*; // opaque; we never dereference

using EGLDisplay = void*;
using EGLContext = void*;
using EGLConfig = void*;
using EGLSurface = void*;
using EGLImageKHR = void*;
using EGLSyncKHR = void*;
using EGLClientBuffer = void*;
using EGLenum = uint32_t;
using EGLint = int32_t;
using EGLBoolean = uint32_t;

using ProcAddressFn = void* (*)(const char*);

// ---- Enumerants (grown per phase; keep sorted-ish by category) ----
// Booleans / misc
inline constexpr GLboolean GL_FALSE = 0;
inline constexpr GLboolean GL_TRUE = 1;
inline constexpr GLenum GL_NO_ERROR = 0;
inline constexpr GLenum GL_NONE = 0;

// getString / getIntegerv queries
inline constexpr GLenum GL_VENDOR = 0x1F00;
inline constexpr GLenum GL_RENDERER = 0x1F01;
inline constexpr GLenum GL_VERSION = 0x1F02;
inline constexpr GLenum GL_EXTENSIONS = 0x1F03;
inline constexpr GLenum GL_SHADING_LANGUAGE_VERSION = 0x8B8C;
inline constexpr GLenum GL_NUM_EXTENSIONS = 0x821D;
inline constexpr GLenum GL_MAX_TEXTURE_SIZE = 0x0D33;
inline constexpr GLenum GL_MAX_TEXTURE_IMAGE_UNITS = 0x8872;
inline constexpr GLenum GL_MAX_SAMPLES = 0x8D57;
inline constexpr GLenum GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT = 0x8A34;
inline constexpr GLenum GL_MAX_UNIFORM_BLOCK_SIZE = 0x8A30;
inline constexpr GLenum GL_MAX_VERTEX_ATTRIBS = 0x8869;
inline constexpr GLenum GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT = 0x84FF; // getIntegerv limit
inline constexpr GLenum GL_TEXTURE_MAX_ANISOTROPY_EXT = 0x84FE;     // per-sampler param

// Capabilities (glEnable/glDisable)
inline constexpr GLenum GL_BLEND = 0x0BE2;
inline constexpr GLenum GL_CULL_FACE = 0x0B44;
inline constexpr GLenum GL_DEPTH_TEST = 0x0B71;
inline constexpr GLenum GL_STENCIL_TEST = 0x0B90;
inline constexpr GLenum GL_SCISSOR_TEST = 0x0C11;
inline constexpr GLenum GL_POLYGON_OFFSET_FILL = 0x8037;
inline constexpr GLenum GL_PRIMITIVE_RESTART_FIXED_INDEX = 0x8D69;
inline constexpr GLenum GL_DITHER = 0x0BD0;
inline constexpr GLenum GL_RASTERIZER_DISCARD = 0x8C89;
inline constexpr GLenum GL_DEBUG_OUTPUT = 0x92E0;
inline constexpr GLenum GL_DEBUG_OUTPUT_SYNCHRONOUS = 0x8242;

// Buffer targets / usage
inline constexpr GLenum GL_ARRAY_BUFFER = 0x8892;
inline constexpr GLenum GL_ELEMENT_ARRAY_BUFFER = 0x8893;
inline constexpr GLenum GL_UNIFORM_BUFFER = 0x8A11;
inline constexpr GLenum GL_PIXEL_PACK_BUFFER = 0x88EB;
inline constexpr GLenum GL_PIXEL_UNPACK_BUFFER = 0x88EC;
inline constexpr GLenum GL_COPY_READ_BUFFER = 0x8F36;
inline constexpr GLenum GL_COPY_WRITE_BUFFER = 0x8F37;
inline constexpr GLenum GL_STATIC_DRAW = 0x88E4;
inline constexpr GLenum GL_DYNAMIC_DRAW = 0x88E8;
inline constexpr GLenum GL_STREAM_DRAW = 0x88E0;
// glMapBufferRange access bits
inline constexpr GLbitfield GL_MAP_READ_BIT = 0x0001;
inline constexpr GLbitfield GL_MAP_WRITE_BIT = 0x0002;
inline constexpr GLbitfield GL_MAP_INVALIDATE_RANGE_BIT = 0x0004;
inline constexpr GLbitfield GL_MAP_INVALIDATE_BUFFER_BIT = 0x0008;
inline constexpr GLbitfield GL_MAP_FLUSH_EXPLICIT_BIT = 0x0010;
inline constexpr GLbitfield GL_MAP_UNSYNCHRONIZED_BIT = 0x0020;
// glMapBufferRange + glBufferStorage(EXT) persistent-mapping bits (GL_EXT_buffer_storage /
// ARB_buffer_storage). A buffer allocated with GL_MAP_PERSISTENT_BIT can stay mapped while the GPU
// uses it; GL_MAP_COHERENT_BIT makes CPU writes visible with no glFlushMappedBufferRange.
inline constexpr GLbitfield GL_MAP_PERSISTENT_BIT = 0x0040;
inline constexpr GLbitfield GL_MAP_COHERENT_BIT = 0x0080;
// glBufferStorage(EXT) storage flags. GL_DYNAMIC_STORAGE_BIT keeps glBufferSubData legal on the
// immutable buffer (the fallback upload path when a persistent mapping could not be obtained).
inline constexpr GLbitfield GL_DYNAMIC_STORAGE_BIT = 0x0100;
inline constexpr GLbitfield GL_CLIENT_STORAGE_BIT = 0x0200;

// Vertex attrib component types
inline constexpr GLenum GL_BYTE = 0x1400;
inline constexpr GLenum GL_UNSIGNED_BYTE = 0x1401;
inline constexpr GLenum GL_SHORT = 0x1402;
inline constexpr GLenum GL_UNSIGNED_SHORT = 0x1403;
inline constexpr GLenum GL_INT = 0x1404;
inline constexpr GLenum GL_UNSIGNED_INT = 0x1405;
inline constexpr GLenum GL_FLOAT = 0x1406;
inline constexpr GLenum GL_HALF_FLOAT = 0x140B;
inline constexpr GLenum GL_UNSIGNED_INT_24_8 = 0x84FA;

// Texture targets / params
inline constexpr GLenum GL_TEXTURE_2D = 0x0DE1;
inline constexpr GLenum GL_TEXTURE0 = 0x84C0;
inline constexpr GLenum GL_TEXTURE_MIN_FILTER = 0x2801;
inline constexpr GLenum GL_TEXTURE_MAG_FILTER = 0x2800;
inline constexpr GLenum GL_TEXTURE_WRAP_S = 0x2802;
inline constexpr GLenum GL_TEXTURE_WRAP_T = 0x2803;
inline constexpr GLenum GL_TEXTURE_WRAP_R = 0x8072;
inline constexpr GLenum GL_TEXTURE_MIN_LOD = 0x813A;
inline constexpr GLenum GL_TEXTURE_MAX_LOD = 0x813B;
inline constexpr GLenum GL_TEXTURE_BASE_LEVEL = 0x813C;
inline constexpr GLenum GL_TEXTURE_MAX_LEVEL = 0x813D;
inline constexpr GLenum GL_TEXTURE_LOD_BIAS = 0x8501;
inline constexpr GLenum GL_TEXTURE_SWIZZLE_R = 0x8E42;
inline constexpr GLenum GL_TEXTURE_SWIZZLE_G = 0x8E43;
inline constexpr GLenum GL_TEXTURE_SWIZZLE_B = 0x8E44;
inline constexpr GLenum GL_TEXTURE_SWIZZLE_A = 0x8E45;
inline constexpr GLint GL_NEAREST = 0x2600;
inline constexpr GLint GL_LINEAR = 0x2601;
inline constexpr GLint GL_NEAREST_MIPMAP_NEAREST = 0x2700;
inline constexpr GLint GL_LINEAR_MIPMAP_NEAREST = 0x2701;
inline constexpr GLint GL_NEAREST_MIPMAP_LINEAR = 0x2702;
inline constexpr GLint GL_LINEAR_MIPMAP_LINEAR = 0x2703;
inline constexpr GLint GL_CLAMP_TO_EDGE = 0x812F;
inline constexpr GLint GL_REPEAT = 0x2901;
inline constexpr GLint GL_MIRRORED_REPEAT = 0x8370;
inline constexpr GLint GL_RED = 0x1903;
inline constexpr GLint GL_GREEN = 0x1904;
inline constexpr GLint GL_BLUE = 0x1905;
inline constexpr GLint GL_ALPHA = 0x1906;
inline constexpr GLint GL_ZERO_SWIZZLE = 0; // GL_ZERO / GL_ONE reused for swizzle
inline constexpr GLenum GL_UNPACK_ALIGNMENT = 0x0CF5;
inline constexpr GLenum GL_UNPACK_ROW_LENGTH = 0x0CF2;
inline constexpr GLenum GL_PACK_ALIGNMENT = 0x0D05;

// Texture / renderbuffer internal + external formats
inline constexpr GLenum GL_RGBA = 0x1908;
inline constexpr GLenum GL_RG = 0x8227;
inline constexpr GLenum GL_RED_INTEGER = 0x8D94;
inline constexpr GLenum GL_DEPTH_COMPONENT = 0x1902;
inline constexpr GLenum GL_DEPTH_STENCIL = 0x84F9;
inline constexpr GLint GL_RGBA8 = 0x8058;
inline constexpr GLint GL_SRGB8_ALPHA8 = 0x8C43;
inline constexpr GLint GL_R8 = 0x8229;
inline constexpr GLint GL_RG8 = 0x822B;
inline constexpr GLint GL_R16I = 0x8233;
inline constexpr GLint GL_R32F = 0x822E;
inline constexpr GLint GL_R32UI = 0x8236;
inline constexpr GLint GL_DEPTH_COMPONENT24 = 0x81A6;
inline constexpr GLint GL_DEPTH_COMPONENT32F = 0x8CAC;
inline constexpr GLint GL_DEPTH24_STENCIL8 = 0x88F0;
// Compressed internalformats (replacement packs only; extension-gated at upload).
// S3TC/RGTC/BPTC are desktop/BC packs; ETC2 is ES 3.0 core; ASTC is KHR_texture_compression_astc_ldr.
inline constexpr GLenum GL_COMPRESSED_RGBA_S3TC_DXT1_EXT = 0x83F1; // BC1
inline constexpr GLenum GL_COMPRESSED_RGBA_S3TC_DXT5_EXT = 0x83F3; // BC3
inline constexpr GLenum GL_COMPRESSED_RED_RGTC1_EXT = 0x8DBB;      // BC4
inline constexpr GLenum GL_COMPRESSED_RG_RGTC2_EXT = 0x8DBD;       // BC5
inline constexpr GLenum GL_COMPRESSED_RGBA_BPTC_UNORM_EXT = 0x8E8C; // BC7
inline constexpr GLenum GL_COMPRESSED_RGB8_ETC2 = 0x9274;
inline constexpr GLenum GL_COMPRESSED_RGBA8_ETC2_EAC = 0x9278;
inline constexpr GLenum GL_COMPRESSED_RGBA_ASTC_4x4_KHR = 0x93B0;
inline constexpr GLenum GL_COMPRESSED_RGBA_ASTC_8x8_KHR = 0x93B7;

// Framebuffers / renderbuffers
inline constexpr GLenum GL_FRAMEBUFFER = 0x8D40;
inline constexpr GLenum GL_READ_FRAMEBUFFER = 0x8CA8;
inline constexpr GLenum GL_DRAW_FRAMEBUFFER = 0x8CA9;
inline constexpr GLenum GL_RENDERBUFFER = 0x8D41;
inline constexpr GLenum GL_COLOR_ATTACHMENT0 = 0x8CE0;
inline constexpr GLenum GL_DEPTH_ATTACHMENT = 0x8D00;
inline constexpr GLenum GL_STENCIL_ATTACHMENT = 0x8D20;
inline constexpr GLenum GL_DEPTH_STENCIL_ATTACHMENT = 0x821A;
inline constexpr GLenum GL_FRAMEBUFFER_COMPLETE = 0x8CD5;
inline constexpr GLbitfield GL_COLOR_BUFFER_BIT = 0x00004000;
inline constexpr GLbitfield GL_DEPTH_BUFFER_BIT = 0x00000100;
inline constexpr GLbitfield GL_STENCIL_BUFFER_BIT = 0x00000400;
// Aspect selectors for glClearBuffer{fv,fi} (distinct from the *_ATTACHMENT enums).
inline constexpr GLenum GL_COLOR = 0x1800;
inline constexpr GLenum GL_DEPTH = 0x1801;
inline constexpr GLenum GL_STENCIL = 0x1802;

// Blend factors / equations
inline constexpr GLenum GL_ZERO = 0;
inline constexpr GLenum GL_ONE = 1;
inline constexpr GLenum GL_SRC_COLOR = 0x0300;
inline constexpr GLenum GL_ONE_MINUS_SRC_COLOR = 0x0301;
inline constexpr GLenum GL_SRC_ALPHA = 0x0302;
inline constexpr GLenum GL_ONE_MINUS_SRC_ALPHA = 0x0303;
inline constexpr GLenum GL_DST_ALPHA = 0x0304;
inline constexpr GLenum GL_ONE_MINUS_DST_ALPHA = 0x0305;
inline constexpr GLenum GL_DST_COLOR = 0x0306;
inline constexpr GLenum GL_ONE_MINUS_DST_COLOR = 0x0307;
inline constexpr GLenum GL_SRC_ALPHA_SATURATE = 0x0308;
inline constexpr GLenum GL_CONSTANT_COLOR = 0x8001;
inline constexpr GLenum GL_ONE_MINUS_CONSTANT_COLOR = 0x8002;
inline constexpr GLenum GL_CONSTANT_ALPHA = 0x8003;
inline constexpr GLenum GL_ONE_MINUS_CONSTANT_ALPHA = 0x8004;
inline constexpr GLenum GL_FUNC_ADD = 0x8006;
inline constexpr GLenum GL_FUNC_SUBTRACT = 0x800A;
inline constexpr GLenum GL_FUNC_REVERSE_SUBTRACT = 0x800B;
inline constexpr GLenum GL_MIN = 0x8007;
inline constexpr GLenum GL_MAX = 0x8008;

// Depth / stencil compare funcs, cull, winding
inline constexpr GLenum GL_NEVER = 0x0200;
inline constexpr GLenum GL_LESS = 0x0201;
inline constexpr GLenum GL_EQUAL = 0x0202;
inline constexpr GLenum GL_LEQUAL = 0x0203;
inline constexpr GLenum GL_GREATER = 0x0204;
inline constexpr GLenum GL_NOTEQUAL = 0x0205;
inline constexpr GLenum GL_GEQUAL = 0x0206;
inline constexpr GLenum GL_ALWAYS = 0x0207;
inline constexpr GLenum GL_KEEP = 0x1E00;
inline constexpr GLenum GL_REPLACE = 0x1E01;
inline constexpr GLenum GL_INCR = 0x1E02;
inline constexpr GLenum GL_INCR_WRAP = 0x8507;
inline constexpr GLenum GL_FRONT = 0x0404;
inline constexpr GLenum GL_BACK = 0x0405;
inline constexpr GLenum GL_CW = 0x0900;
inline constexpr GLenum GL_CCW = 0x0901;

// Primitive types
inline constexpr GLenum GL_POINTS = 0x0000;
inline constexpr GLenum GL_LINES = 0x0001;
inline constexpr GLenum GL_LINE_STRIP = 0x0003;
inline constexpr GLenum GL_TRIANGLES = 0x0004;
inline constexpr GLenum GL_TRIANGLE_STRIP = 0x0005;

// Shaders / programs
inline constexpr GLenum GL_FRAGMENT_SHADER = 0x8B30;
inline constexpr GLenum GL_VERTEX_SHADER = 0x8B31;
inline constexpr GLenum GL_COMPILE_STATUS = 0x8B81;
inline constexpr GLenum GL_LINK_STATUS = 0x8B82;
inline constexpr GLenum GL_INFO_LOG_LENGTH = 0x8B84;
inline constexpr GLenum GL_UNIFORM_BLOCK_DATA_SIZE = 0x8A40;

// Program binary (ES 3.0 core: glGetProgramBinary/glProgramBinary, the persistent
// shader-cache path). RETRIEVABLE_HINT must be set before link for some drivers to
// keep the binary retrievable.
inline constexpr GLenum GL_PROGRAM_BINARY_RETRIEVABLE_HINT = 0x8257;
inline constexpr GLenum GL_PROGRAM_BINARY_LENGTH = 0x8741;
inline constexpr GLenum GL_NUM_PROGRAM_BINARY_FORMATS = 0x87FE;
inline constexpr GLenum GL_PROGRAM_BINARY_FORMATS = 0x87FF;

// Sync
inline constexpr GLenum GL_SYNC_GPU_COMMANDS_COMPLETE = 0x9117;
inline constexpr GLenum GL_ALREADY_SIGNALED = 0x911A;
inline constexpr GLenum GL_TIMEOUT_EXPIRED = 0x911B;
inline constexpr GLenum GL_CONDITION_SATISFIED = 0x911C;
inline constexpr GLenum GL_WAIT_FAILED = 0x911D;
inline constexpr GLbitfield GL_SYNC_FLUSH_COMMANDS_BIT = 0x00000001;
inline constexpr GLuint64 GL_TIMEOUT_IGNORED = 0xFFFFFFFFFFFFFFFFull;

// EGL
inline constexpr EGLint EGL_NONE = 0x3038;
inline constexpr EGLint EGL_NO_CONTEXT = 0;
inline constexpr EGLenum EGL_GL_TEXTURE_2D_KHR = 0x30B1;
inline constexpr EGLenum EGL_GL_TEXTURE_LEVEL_KHR = 0x30BC;
inline constexpr EGLenum EGL_IMAGE_PRESERVED_KHR = 0x30D2;
inline constexpr EGLenum EGL_SYNC_FENCE_KHR = 0x30F9;
inline constexpr EGLint EGL_CONDITION_SATISFIED_KHR = 0x30F6;
inline constexpr EGLint EGL_FOREVER_KHR = 0x7FFFFFFF;
inline constexpr EGLenum EGL_CONTEXT_CLIENT_VERSION = 0x3098;
inline constexpr EGLenum EGL_WIDTH = 0x3057;
inline constexpr EGLenum EGL_HEIGHT = 0x3056;
// Config selection for the device (sdl2-shim) render/share contexts' dummy pbuffers.
inline constexpr EGLBoolean EGL_TRUE = 1;
inline constexpr EGLBoolean EGL_FALSE = 0;
inline constexpr EGLSurface EGL_NO_SURFACE = nullptr;
inline constexpr EGLenum EGL_SURFACE_TYPE = 0x3033;
inline constexpr EGLint EGL_PBUFFER_BIT = 0x0001;
inline constexpr EGLenum EGL_RENDERABLE_TYPE = 0x3040;
inline constexpr EGLint EGL_OPENGL_ES2_BIT = 0x0004;
inline constexpr EGLint EGL_OPENGL_ES3_BIT = 0x0040; // == EGL_OPENGL_ES3_BIT_KHR
inline constexpr EGLenum EGL_RED_SIZE = 0x3024;
inline constexpr EGLenum EGL_GREEN_SIZE = 0x3023;
inline constexpr EGLenum EGL_BLUE_SIZE = 0x3022;
inline constexpr EGLenum EGL_ALPHA_SIZE = 0x3021;
inline constexpr EGLenum EGL_DEPTH_SIZE = 0x3025;
inline constexpr EGLenum EGL_STENCIL_SIZE = 0x3026;

// The resolved entry-point table. Members are plain function pointers, filled
// by gl::load(). Names match the GL/EGL entry points with the gl/egl prefix
// dropped. A pointer left null means the driver lacks that entry point; callers
// gate optional ones (debug, aniso, EGLImage) on a non-null check.
struct GlProcTable {
  // Buffers
  void (*GenBuffers)(GLsizei, GLuint*) = nullptr;
  void (*DeleteBuffers)(GLsizei, const GLuint*) = nullptr;
  void (*BindBuffer)(GLenum, GLuint) = nullptr;
  void (*BufferData)(GLenum, GLsizeiptr, const void*, GLenum) = nullptr;
  void (*BufferSubData)(GLenum, GLintptr, GLsizeiptr, const void*) = nullptr;
  void (*BindBufferRange)(GLenum, GLuint, GLuint, GLintptr, GLsizeiptr) = nullptr;
  void (*BindBufferBase)(GLenum, GLuint, GLuint) = nullptr;
  void* (*MapBufferRange)(GLenum, GLintptr, GLsizeiptr, GLbitfield) = nullptr;
  GLboolean (*UnmapBuffer)(GLenum) = nullptr;
  void (*FlushMappedBufferRange)(GLenum, GLintptr, GLsizeiptr) = nullptr;
  void (*CopyBufferSubData)(GLenum, GLenum, GLintptr, GLintptr, GLsizeiptr) = nullptr;
  // GL_EXT_buffer_storage (glBufferStorageEXT) -- the device persistent-mapped write-combine fast
  // path. Null when unsupported; callers fall back to glBufferData + glBufferSubData.
  void (*BufferStorage)(GLenum, GLsizeiptr, const void*, GLbitfield) = nullptr;

  // Vertex arrays
  void (*GenVertexArrays)(GLsizei, GLuint*) = nullptr;
  void (*DeleteVertexArrays)(GLsizei, const GLuint*) = nullptr;
  void (*BindVertexArray)(GLuint) = nullptr;
  void (*EnableVertexAttribArray)(GLuint) = nullptr;
  void (*DisableVertexAttribArray)(GLuint) = nullptr;
  void (*VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*) = nullptr;
  void (*VertexAttribIPointer)(GLuint, GLint, GLenum, GLsizei, const void*) = nullptr;

  // Textures
  void (*GenTextures)(GLsizei, GLuint*) = nullptr;
  void (*DeleteTextures)(GLsizei, const GLuint*) = nullptr;
  void (*BindTexture)(GLenum, GLuint) = nullptr;
  void (*ActiveTexture)(GLenum) = nullptr;
  void (*TexStorage2D)(GLenum, GLsizei, GLenum, GLsizei, GLsizei) = nullptr;
  void (*TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*) = nullptr;
  void (*TexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*) = nullptr;
  void (*CompressedTexImage2D)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void*) = nullptr;
  void (*CompressedTexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLsizei,
                                  const void*) = nullptr;
  void (*CopyTexSubImage2D)(GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei) = nullptr;
  void (*TexParameteri)(GLenum, GLenum, GLint) = nullptr;
  void (*TexParameterf)(GLenum, GLenum, GLfloat) = nullptr;
  void (*GenerateMipmap)(GLenum) = nullptr;
  void (*PixelStorei)(GLenum, GLint) = nullptr;

  // Sampler objects
  void (*GenSamplers)(GLsizei, GLuint*) = nullptr;
  void (*DeleteSamplers)(GLsizei, const GLuint*) = nullptr;
  void (*BindSampler)(GLuint, GLuint) = nullptr;
  void (*SamplerParameteri)(GLuint, GLenum, GLint) = nullptr;
  void (*SamplerParameterf)(GLuint, GLenum, GLfloat) = nullptr;

  // Framebuffers / renderbuffers
  void (*GenFramebuffers)(GLsizei, GLuint*) = nullptr;
  void (*DeleteFramebuffers)(GLsizei, const GLuint*) = nullptr;
  void (*BindFramebuffer)(GLenum, GLuint) = nullptr;
  void (*FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint) = nullptr;
  void (*FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint) = nullptr;
  GLenum (*CheckFramebufferStatus)(GLenum) = nullptr;
  void (*BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum) = nullptr;
  void (*InvalidateFramebuffer)(GLenum, GLsizei, const GLenum*) = nullptr;
  void (*DrawBuffers)(GLsizei, const GLenum*) = nullptr;
  void (*ReadBuffer)(GLenum) = nullptr;
  void (*ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*) = nullptr;
  void (*GenRenderbuffers)(GLsizei, GLuint*) = nullptr;
  void (*DeleteRenderbuffers)(GLsizei, const GLuint*) = nullptr;
  void (*BindRenderbuffer)(GLenum, GLuint) = nullptr;
  void (*RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei) = nullptr;
  void (*RenderbufferStorageMultisample)(GLenum, GLsizei, GLenum, GLsizei, GLsizei) = nullptr;

  // Clears
  void (*ClearColor)(GLclampf, GLclampf, GLclampf, GLclampf) = nullptr;
  void (*ClearDepthf)(GLclampf) = nullptr;
  void (*ClearStencil)(GLint) = nullptr;
  void (*Clear)(GLbitfield) = nullptr;
  void (*ClearBufferfv)(GLenum, GLint, const GLfloat*) = nullptr;
  void (*ClearBufferfi)(GLenum, GLint, GLfloat, GLint) = nullptr;
  void (*ClearBufferiv)(GLenum, GLint, const GLint*) = nullptr;
  void (*ClearBufferuiv)(GLenum, GLint, const GLuint*) = nullptr;

  // Shaders / programs
  GLuint (*CreateShader)(GLenum) = nullptr;
  void (*ShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*) = nullptr;
  void (*CompileShader)(GLuint) = nullptr;
  void (*GetShaderiv)(GLuint, GLenum, GLint*) = nullptr;
  void (*GetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
  void (*DeleteShader)(GLuint) = nullptr;
  GLuint (*CreateProgram)() = nullptr;
  void (*AttachShader)(GLuint, GLuint) = nullptr;
  void (*BindAttribLocation)(GLuint, GLuint, const GLchar*) = nullptr;
  void (*LinkProgram)(GLuint) = nullptr;
  void (*GetProgramiv)(GLuint, GLenum, GLint*) = nullptr;
  void (*GetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
  void (*UseProgram)(GLuint) = nullptr;
  void (*DeleteProgram)(GLuint) = nullptr;
  GLint (*GetUniformLocation)(GLuint, const GLchar*) = nullptr;
  void (*Uniform1i)(GLint, GLint) = nullptr;
  GLuint (*GetUniformBlockIndex)(GLuint, const GLchar*) = nullptr;
  void (*UniformBlockBinding)(GLuint, GLuint, GLuint) = nullptr;
  void (*GetActiveUniformBlockiv)(GLuint, GLuint, GLenum, GLint*) = nullptr;
  // Program binary (optional -- gated on GL_NUM_PROGRAM_BINARY_FORMATS > 0)
  void (*GetProgramBinary)(GLuint, GLsizei, GLsizei*, GLenum*, void*) = nullptr;
  void (*ProgramBinary)(GLuint, GLenum, const void*, GLsizei) = nullptr;
  void (*ProgramParameteri)(GLuint, GLenum, GLint) = nullptr;

  // Fixed-function state
  void (*Enable)(GLenum) = nullptr;
  void (*Disable)(GLenum) = nullptr;
  void (*BlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum) = nullptr;
  void (*BlendEquationSeparate)(GLenum, GLenum) = nullptr;
  void (*BlendColor)(GLclampf, GLclampf, GLclampf, GLclampf) = nullptr;
  void (*ColorMask)(GLboolean, GLboolean, GLboolean, GLboolean) = nullptr;
  void (*DepthMask)(GLboolean) = nullptr;
  void (*DepthFunc)(GLenum) = nullptr;
  void (*DepthRangef)(GLclampf, GLclampf) = nullptr;
  void (*CullFace)(GLenum) = nullptr;
  void (*FrontFace)(GLenum) = nullptr;
  void (*PolygonOffset)(GLfloat, GLfloat) = nullptr;
  void (*Viewport)(GLint, GLint, GLsizei, GLsizei) = nullptr;
  void (*Scissor)(GLint, GLint, GLsizei, GLsizei) = nullptr;
  void (*StencilFuncSeparate)(GLenum, GLenum, GLint, GLuint) = nullptr;
  void (*StencilOpSeparate)(GLenum, GLenum, GLenum, GLenum) = nullptr;
  void (*StencilMaskSeparate)(GLenum, GLuint) = nullptr;

  // Draw
  void (*DrawArrays)(GLenum, GLint, GLsizei) = nullptr;
  void (*DrawElements)(GLenum, GLsizei, GLenum, const void*) = nullptr;
  void (*DrawArraysInstanced)(GLenum, GLint, GLsizei, GLsizei) = nullptr;
  void (*DrawElementsInstanced)(GLenum, GLsizei, GLenum, const void*, GLsizei) = nullptr;

  // Sync / query
  GLsync (*FenceSync)(GLenum, GLbitfield) = nullptr;
  GLenum (*ClientWaitSync)(GLsync, GLbitfield, GLuint64) = nullptr;
  void (*WaitSync)(GLsync, GLbitfield, GLuint64) = nullptr;
  void (*DeleteSync)(GLsync) = nullptr;
  void (*Finish)() = nullptr;
  void (*Flush)() = nullptr;
  GLenum (*GetError)() = nullptr;
  const GLubyte* (*GetString)(GLenum) = nullptr;
  const GLubyte* (*GetStringi)(GLenum, GLuint) = nullptr;
  void (*GetIntegerv)(GLenum, GLint*) = nullptr;
  void (*GetFloatv)(GLenum, GLfloat*) = nullptr;

  // Debug (KHR_debug; optional)
  void (*DebugMessageCallback)(void*, const void*) = nullptr;
  void (*DebugMessageControl)(GLenum, GLenum, GLenum, GLsizei, const GLuint*, GLboolean) = nullptr;
  void (*PushDebugGroup)(GLenum, GLuint, GLsizei, const GLchar*) = nullptr;
  void (*PopDebugGroup)() = nullptr;
  void (*ObjectLabel)(GLenum, GLuint, GLsizei, const GLchar*) = nullptr;

  // EGL (device path: context, image interop, fences)
  EGLContext (*eglGetCurrentContext)() = nullptr;
  EGLDisplay (*eglGetCurrentDisplay)() = nullptr;
  EGLSurface (*eglGetCurrentSurface)(EGLint) = nullptr;
  EGLBoolean (*eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) = nullptr;
  EGLContext (*eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint*) = nullptr;
  EGLBoolean (*eglDestroyContext)(EGLDisplay, EGLContext) = nullptr;
  EGLSurface (*eglCreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint*) = nullptr;
  EGLBoolean (*eglDestroySurface)(EGLDisplay, EGLSurface) = nullptr;
  EGLBoolean (*eglChooseConfig)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*) = nullptr;
  EGLBoolean (*eglGetConfigAttrib)(EGLDisplay, EGLConfig, EGLint, EGLint*) = nullptr;
  EGLint (*eglGetError)() = nullptr;
  const char* (*eglQueryString)(EGLDisplay, EGLint) = nullptr;
  EGLImageKHR (*eglCreateImageKHR)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint*) = nullptr;
  EGLBoolean (*eglDestroyImageKHR)(EGLDisplay, EGLImageKHR) = nullptr;
  EGLSyncKHR (*eglCreateSyncKHR)(EGLDisplay, EGLenum, const EGLint*) = nullptr;
  EGLBoolean (*eglDestroySyncKHR)(EGLDisplay, EGLSyncKHR) = nullptr;
  EGLint (*eglClientWaitSyncKHR)(EGLDisplay, EGLSyncKHR, EGLint, uint64_t) = nullptr;
  EGLBoolean (*eglWaitSyncKHR)(EGLDisplay, EGLSyncKHR, EGLint) = nullptr;
  void (*glEGLImageTargetTexture2DOES)(GLenum, void*) = nullptr;
};

// The one global proc table. Filled by gl::load(); read everywhere via `gl`.
extern GlProcTable gl;

// Resolve every entry point in `gl` through `getProc` (with a libGLESv2/libEGL
// dlsym fallback for symbols the shim getProc refuses, mirroring
// gpu.cpp's sdl2shim_egl_get_proc). Returns false if a REQUIRED core entry point
// is missing (optional ones -- debug/aniso/EGLImage -- may stay null). Must be
// called with the owning context current on the calling thread.
bool load(ProcAddressFn getProc);

// True once load() has populated the required core set.
bool loaded();

} // namespace aurora::gl
