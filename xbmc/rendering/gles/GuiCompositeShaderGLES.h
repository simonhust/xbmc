/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "guilib/Shader.h"

#include <string>

class CGuiCompositeShaderGLES : public Shaders::CGLSLShaderProgram
{
public:
  explicit CGuiCompositeShaderGLES(const std::string& prefix);
  ~CGuiCompositeShaderGLES() override = default;

  void SetProjection(const GLfloat* proj) { m_proj = proj; }

  GLint GetPosLoc() { return m_hPos; }
  GLint GetTexLoc() { return m_hTex; }

protected:
  void OnCompiledAndLinked() override;
  bool OnEnabled() override;

private:
  const GLfloat* m_proj{nullptr};

  GLint m_hPos{-1};
  GLint m_hTex{-1};
  GLint m_hSamp{-1};
  GLint m_hProj{-1};
};