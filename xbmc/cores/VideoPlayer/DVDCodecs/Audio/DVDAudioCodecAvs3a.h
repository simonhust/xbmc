#pragma once

#include "DVDAudioCodec.h"

#include <memory>
#include <string>
#include <vector>

class CProcessInfo;

struct avs3a_decoder_ctx;

class CDVDAudioCodecAvs3a : public CDVDAudioCodec
{
public:
  CDVDAudioCodecAvs3a(CProcessInfo &processInfo);
  ~CDVDAudioCodecAvs3a() override;

  bool Open(CDVDStreamInfo &hints, CDVDCodecOptions &options) override;
  void Dispose() override;
  bool AddData(const DemuxPacket &packet) override;
  void GetData(DVDAudioFrame &frame) override;
  void Reset() override;
  AEAudioFormat GetFormat() override { return m_format; }
  std::string GetName() override { return m_codecName; }

  static std::unique_ptr<CDVDAudioCodec> Create(CProcessInfo &processInfo);
  static bool Register();

private:
  avs3a_decoder_ctx* m_ctx = nullptr;
  AEAudioFormat m_format;
  std::string m_codecName;
  std::vector<int16_t> m_outputBuffer;
  int m_samplesPerFrame = 0;
  bool m_hasOutput = false;
};
