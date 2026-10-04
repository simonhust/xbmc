set(SOURCES DVDAudioCodecFFmpeg.cpp
            DVDAudioCodecPassthrough.cpp
            DVDAudioCodecAvs3a.cpp
            DVDAudioCodecDtsUhd.cpp)

set(HEADERS DVDAudioCodec.h
            DVDAudioCodecFFmpeg.h
            DVDAudioCodecPassthrough.h
            DVDAudioCodecAvs3a.h
            DVDAudioCodecDtsUhd.h
            FloatingAverage.h)

if(CORE_SYSTEM_NAME STREQUAL android)
  list(APPEND SOURCES DVDAudioCodecAndroidMediaCodec.cpp)
  list(APPEND HEADERS DVDAudioCodecAndroidMediaCodec.h)
endif()

find_library(AVS3A_DECODER_LIBRARY NAMES avs3Decoder_NEON PATHS "${SYSROOT_PREFIX}/usr/lib" NO_DEFAULT_PATH)
find_library(DTS_UHD_DECODER_LIBRARY NAMES dts_uhd_decoder PATHS "${SYSROOT_PREFIX}/usr/lib" NO_DEFAULT_PATH)

core_add_library(dvdaudiocodecs)
if(AVS3A_DECODER_LIBRARY)
  target_link_libraries(dvdaudiocodecs ${AVS3A_DECODER_LIBRARY})
endif()
if(DTS_UHD_DECODER_LIBRARY)
  target_link_libraries(dvdaudiocodecs ${DTS_UHD_DECODER_LIBRARY})
endif()
