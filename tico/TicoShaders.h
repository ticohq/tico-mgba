#pragma once

#include "TicoCore.h" // for ShaderType enum

/// Returns the GLSL 330 core vertex shader source used for the fullscreen quad.
const char* GetShaderVertexSource();

/// Returns the GLSL 330 core fragment shader source for the given ShaderType.
const char* GetShaderFragmentSource(ShaderType type);
