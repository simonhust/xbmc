/*
 *      Copyright (C) 2010-2013 Team XBMC
 *      http://xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#version 100

precision mediump float;
uniform sampler2D m_samp0;
uniform lowp vec4 m_unicol;
varying vec4 m_cord0;
uniform float m_sdrPeak;
uniform float m_sdrSaturation;

void main ()
{
  vec4 rgb;

  rgb = texture2D(m_samp0, m_cord0.xy).rgba * m_unicol;

#if defined(KODI_LIMITED_RANGE)
  rgb.rgb *= (235.0 - 16.0) / 255.0;
  rgb.rgb += 16.0 / 255.0;
#endif

#if defined(KODI_TRANSFER_PQ)
  // BT.709 -> BT.2020 gamut conversion in the sRGB-encoded domain. The transfer
  // function (sRGB -> linear -> PQ) is applied later by the OSD HDR core.
  const mat3 bt709_to_bt2020 = mat3(0.6274, 0.0691, 0.0164, 0.3293, 0.9195, 0.0880,
                                    0.0433, 0.0114, 0.8956);
  rgb.rgb = bt709_to_bt2020 * rgb.rgb;
  vec3 luma = vec3(dot(rgb.rgb, vec3(0.2627, 0.6780, 0.0593)));
  rgb.rgb = mix(luma, rgb.rgb, m_sdrSaturation);
#endif

  gl_FragColor = rgb;
}
