set(SOURCES DVDAudioCodecFFmpeg.cpp
            DVDAudioCodecPassthrough.cpp)

set(HEADERS DVDAudioCodec.h
            DVDAudioCodecFFmpeg.h
            DVDAudioCodecPassthrough.h
            FloatingAverage.h)

if(CORE_SYSTEM_NAME STREQUAL android)
  list(APPEND SOURCES DVDAudioCodecAndroidMediaCodec.cpp)
  list(APPEND HEADERS DVDAudioCodecAndroidMediaCodec.h)
endif()

find_path(AVS3A_DECODER_INCLUDE_DIR NAMES avs3a/avs3a_wrapper.h PATHS "${SYSROOT_PREFIX}/usr/include" NO_DEFAULT_PATH)
find_library(AVS3A_DECODER_LIBRARY NAMES avs3Decoder_NEON PATHS "${SYSROOT_PREFIX}/usr/lib" NO_DEFAULT_PATH)
find_path(DTS_UHD_DECODER_INCLUDE_DIR NAMES dts_uhd/dts_uhd_wrapper.h PATHS "${SYSROOT_PREFIX}/usr/include" NO_DEFAULT_PATH)
find_library(DTS_UHD_DECODER_LIBRARY NAMES dts_uhd_decoder PATHS "${SYSROOT_PREFIX}/usr/lib" NO_DEFAULT_PATH)

if(AVS3A_DECODER_INCLUDE_DIR AND AVS3A_DECODER_LIBRARY)
  list(APPEND SOURCES DVDAudioCodecAvs3a.cpp)
  list(APPEND HEADERS DVDAudioCodecAvs3a.h)
  get_filename_component(_AVS3A_INC "${AVS3A_DECODER_INCLUDE_DIR}" DIRECTORY)
  set(AVS3A_DECODER_FOUND 1)
endif()

if(DTS_UHD_DECODER_INCLUDE_DIR AND DTS_UHD_DECODER_LIBRARY)
  list(APPEND SOURCES DVDAudioCodecDtsUhd.cpp)
  list(APPEND HEADERS DVDAudioCodecDtsUhd.h)
  get_filename_component(_DTSUHD_INC "${DTS_UHD_DECODER_INCLUDE_DIR}" DIRECTORY)
  set(DTS_UHD_DECODER_FOUND 1)
endif()

core_add_library(dvdaudiocodecs)
if(AVS3A_DECODER_FOUND)
  target_include_directories(dvdaudiocodecs PRIVATE ${_AVS3A_INC})
  target_link_libraries(dvdaudiocodecs ${AVS3A_DECODER_LIBRARY})
endif()
if(DTS_UHD_DECODER_FOUND)
  target_include_directories(dvdaudiocodecs PRIVATE ${_DTSUHD_INC})
  target_link_libraries(dvdaudiocodecs ${DTS_UHD_DECODER_LIBRARY})
endif()
