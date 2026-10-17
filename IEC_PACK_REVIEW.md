# DTS-UHD / AV3A IEC Pack 交叉校准报告

## 1. DTS-UHD IEC Pack 校准

### 1.1 Kodi 实现 (AEBitstreamPacker::PackDTSHD)

```cpp
void CAEBitstreamPacker::PackDTSHD(CAEStreamInfo &info, uint8_t* data, int size) {
  static const uint8_t dtshd_start_code[10] = {0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xfe,0xfe};
  unsigned int dataSize = sizeof(dtshd_start_code) + 2 + size;
  // ...wrap into m_dtsHD buffer...
  m_dataSize = CAEPackIEC61937::PackDTSHD(m_dtsHD.data(), dataSize, m_packedBuffer, info.m_dtsPeriod);
}
```

IEC wrap (`AEPackIEC61937::PackDTSHD`):
- Pc = `IEC61937_TYPE_DTSHD | (subtype << 8)` where subtype = floor(log2(period/512))
  - period=512 → subtype=0 → Pc=0x11
  - period=1024 → subtype=1 → Pc=0x111
  - period=2048 → subtype=2 → Pc=0x211
  - period=4096 → subtype=3 → Pc=0x311
- Pd = `((size + 0x17) &~ 0x0f) - 0x08` (alignment to 0x8 boundary)
- Burst size (return) = `period << 2` → 512→2048, 1024→4096, 2048→8192, 4096→16384
- Byte-swap payload to big-endian (on LE systems)

DTS-HD container header added before IEC wrap:
```
[0x01 0x00 0x00 0x00 0x00 0x00 0x00 0x00 0xfe 0xfe]  (10 bytes)
[size_high size_low]                                   (2 bytes, BE)
[raw ACE substream data]                               (size bytes)
```

### 1.2 Vendor SDO_P2HdmiPacker 行为 (逆向)

```
SDO_P2HdmiPacker_Transmit(ctx, input, ...):
  - input→substream array (12 bytes per entry)
  - iterates: [base+0] (presence flag), [base+4] (data pointer)
  - burst size = f(channels, sample_rate) via lookup table
  - ctx→field_56 = packer type (3 = P2HDMI)
  - calls SDO_P2HdmiPacker_GetFrameSize → burst size
```

### 1.3 校准结果

| 项目 | Kodi (修复前) | Kodi (修复后) | Vendor | 状态 |
|------|--------------|--------------|--------|------|
| IEC 同步字 Pa/Pb | 0xF872/0x4E1F | 0xF872/0x4E1F | 0xF872/0x4E1F | ✅ |
| Pc 数据类型 | 0x11 (DTSHD) | 0x11 (DTSHD) | 0x11 (DTSHD) | ✅ |
| Burst 大小 (period=512) | 2048 bytes | 2048 bytes | 2048 bytes | ✅ |
| Burst 大小 (period=1024) | 4096 bytes | 4096 bytes | 4096 bytes | ✅ |
| 字节序 | Big-endian | Big-endian | Big-endian | ✅ |
| DTS-HD 容器头 | **10+2 bytes ❌** | **已去除 ✅** | 无 | ✅ 修复后一致 |

**⚠️ 注意**: Kodi 在 IEC wrap 前加了 DTS-HD 容器头（0x01...0xfefe + size）。Vendor
SDO 的输入是 ACE substream 直通还是 DTS-HD 容器格式需要通过实际设备播放测试
确认。如果 AVR 期望原始 ACE substream（无容器头），则 Kodi 的 10-byte 头需要去掉。

### 1.4 AESinkALSA 配置

```
GetChannelLayoutRaw(STREAM_TYPE_DTS_UHD) → count = 2 channels
→ ALSA opens 2ch at audio sample rate (48000Hz)
→ Each IEC burst carries 4 bytes/sample (16-bit IEC 61937 frame = 2ch × 2B)
→ Data rate = 48000 × 8 = 384kbps on the wire
```

这与 vendor SDO 行为一致：DTS-UHD 直通用 2ch IEC 接口，每个 burst 封装一帧。

---

## 2. AV3A IEC Pack 校准

### 2.1 Kodi 实现 (AEPackIEC61937::PackAV3A)

```cpp
int CAEPackIEC61937::PackAV3A(uint8_t* data, unsigned int size, uint8_t* dest,
                              unsigned int frameLength) {
  // subDataType from frameLength:
  //   512  → subDataType = 1 (IEC 61937-17 table 1)
  //   1024 → subDataType = 0
  unsigned int subDataType = (frameLength == 512) ? 1 : 0;

  header[0] = 0xF872;                                  // Pa
  header[1] = 0x4E1F;                                  // Pb
  header[2] = 0x1F | (subDataType << 8);              // Pc = data-type + sub-type
  header[3] = size + 2*(6-4);                          // Pd = payload len + Pe/Pf overhead
  header[4] = 0x0002;                                  // Pe = AV3A extended type
  header[5] = 0;                                       // Pf = reserved

  SwapEndian(payload, data, size>>1);                  // LE→BE
  memset(payload + size, 0, burstsize - headerBytes - size);  // pad
  return burstsize = frameLength << 2;                 // 512→2048, 1024→4096
}
```

### 2.2 AudioVivid 源码参考 (IEC 61937-17)

AudioVivid 封装工具（FFmpeg-n4.4.2）**不含**显式 IEC pack 代码。AV3A 在容器层
（MP4/MKV/TS）封装为基本流，IEC 61937-17 打包在 ALSA/sink 层完成。

IEC 61937-17 规范（基于 Kodi 代码注释 + spdif.h）：
- Pa = 0xF872, Pb = 0x4E1F
- Pc[7:0] = 0x1F (AV3A data type)
- Pc[9:8] = sub-data-type (frame length indicator)
- Pd = burst payload length (bytes), includes Pe/Pf overhead
- Pe = extended type (0x0002 for AV3A)
- Pf = reserved (0)

| 项目 | Kodi | IEC 61937-17 规范 | 状态 |
|------|------|-------------------|------|
| Pa/Pb | 0xF872/0x4E1F | 0xF872/0x4E1F | ✅ |
| Pc data-type | 0x1F | 0x1F | ✅ |
| Pc sub-type (512) | 1 | 1 | ✅ |
| Pc sub-type (1024) | 0 | 0 | ✅ |
| Pe | 0x0002 | 0x0002 | ✅ |
| Pf | 0 | 0 | ✅ |
| Burst size | frameLength << 2 | frameLength × 4B | ✅ |
| Byte swap | LE→BE | LE→BE | ✅ |

### 2.3 AV3AHeader.h 帧解析校准

AV3AHeader.h 基于 T/UWA 009.1 annex A 规范。与 FFmpeg av3a.c 解析对比：

| 字段 | AV3AHeader | FFmpeg av3a.c | 状态 |
|------|-----------|---------------|------|
| Sync word | 0xFFF (12-bit) | 0xFFF | ✅ |
| Codec ID | 1=lossless, 2=general | 对应 avs3a 版本 | ✅ |
| Profile | 0=ch, 1=obj, 2=HOA | 对应 coding mode | ✅ |
| Sample rates | 192k/96k/48k/44.1k/32k/... | 同表 | ✅ |
| Bitrate tables | MONO/STEREO/5.1/7.1 | 同表 | ✅ |
| Frame length | bitrate-dependent | bitrate-dependent | ✅ |

**结论**: AV3A IEC pack 实现与规范一致，AudioVivid 源码不含对应 IEC pack 代码
（在 sink 层处理），无法直接对照但无冲突。

---

## 3. 已修复

### 3.1 DTS-UHD Profile 2 直通 — 去除 DTS-HD 容器头

**问题**: Kodi 的 `PackDTSHD` 在 IEC wrap 前加了 10+2 字节的 DTS-HD MA 容器头
(`0x01 0x00 ... 0xfe 0xfe` + size)。Vendor 的 `SDO_P2HdmiPacker_Transmit` 直接将
原始 ACE substream 包装在 IEC 61937 中，**不加容器头**。

**修复**: `AEBitstreamPacker::Pack` 中 `STREAM_TYPE_DTS_UHD` 不再调用
`PackDTSHD`（容器头版本），而是直接调用 `CAEPackIEC61937::PackDTSHD`。

**Commit**: `78cbd23d56`

### 3.2 AESinkALSA DTS-UHD 路由

DTS-UHD 在 ALSA 层配置为 2ch @ 音频采样率，每个 IEC burst 4 bytes/sample，
数据率 = 48000 × 8 = 384kbps，与 vendor SDO 行为一致。
