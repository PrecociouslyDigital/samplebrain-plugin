set(upstream "${SAMPLEBRAIN_SRC}/brain/src")

# Upstream's engine, minus OSC, PortAudio, the mic stream and its main.
add_library(samplebrain_upstream STATIC
  ${upstream}/block.cpp
  ${upstream}/brain.cpp
  ${upstream}/fft.cpp
  ${upstream}/mfcc.cpp
  ${upstream}/renderer.cpp
  ${upstream}/search_params.cpp
  ${upstream}/window.cpp
  ${upstream}/aquila/filter/MelFilterBank.cpp
  ${upstream}/aquila/filter/MelFilter.cpp
  ${upstream}/aquila/transform/Dct.cpp
  ${upstream}/spiralcore/sample.cpp
  ${upstream}/spiralcore/allocator.cpp
  ${upstream}/spiralcore/stream.cpp
  status.cpp
)
target_include_directories(samplebrain_upstream SYSTEM PUBLIC ${upstream})
target_include_directories(samplebrain_upstream PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# status.h includes liblo's headers, but nothing calls into liblo.
target_include_directories(samplebrain_upstream SYSTEM PUBLIC ${liblo_INCLUDE_DIRS})
target_link_libraries(samplebrain_upstream PUBLIC PkgConfig::fftw3 PkgConfig::sndfile)
target_compile_options(samplebrain_upstream PRIVATE -w -O3)
set_target_properties(samplebrain_upstream PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(samplebrain_engine STATIC
  core/core.cpp
  core/keys.cpp
  core/progress.cpp
  io/audio_file.cpp
  io/render_file.cpp
  cache/disk_cache.cpp
)
target_include_directories(samplebrain_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(samplebrain_engine
  PUBLIC PkgConfig::xxhash
  PRIVATE samplebrain_upstream PkgConfig::sndfile PkgConfig::samplerate)
target_compile_options(samplebrain_engine PRIVATE -Wall -Wextra)
set_target_properties(samplebrain_engine PROPERTIES POSITION_INDEPENDENT_CODE ON)
