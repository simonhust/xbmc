/*
 *  Copyright (C) 2024 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#version 100

precision mediump float;
uniform sampler2D m_samp0;
uniform sampler2D m_samp1;
varying vec4 m_cord0;
varying vec4 m_cord1;
uniform lowp vec4 m_unicol;
uniform float m_sdrSaturation;

void main()
{
  gl_FragColor = m_unicol;
  gl_FragColor *= texture2D(m_samp0, m_cord0.xy);
  gl_FragColor.a *= texture2D(m_samp1, m_cord1.xy).r;

#if defined(KODI_LIMITED_RANGE)
  gl_FragColor.rgb *= (235.0 - 16.0) / 255.0;
  gl_FragColor.rgb += 16.0 / 255.0;
#endif

#if defined(KODI_TRANSFER_PQ)
  // BT.709 -> BT.2020 gamut conversion in the sRGB-encoded domain. The transfer
  // function (sRGB -> linear -> PQ) is applied later by the OSD HDR core.
  const mat3 bt709_to_bt2020 = mat3(0.6274, 0.0691, 0.0164, 0.3293, 0.9195, 0.0880,
                                    0.0433, 0.0114, 0.8956);
  gl_FragColor.rgb = bt709_to_bt2020 * gl_FragColor.rgb;
  const vec3 luma = vec3(dot(gl_FragColor.rgb, vec3(0.2627, 0.6780, 0.0593)));
  gl_FragColor.rgb = mix(luma, gl_FragColor.rgb, m_sdrSaturation);
#endif
}
