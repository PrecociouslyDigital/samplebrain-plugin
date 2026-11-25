find_package(Catch2 3 REQUIRED)
include(Catch)

add_executable(engine_tests
  cache_tests.cpp
  playback_tests.cpp
  published_tests.cpp
  rate_tests.cpp
  render_tests.cpp
  settings_tests.cpp
)
target_link_libraries(engine_tests PRIVATE samplebrain_engine Catch2::Catch2WithMain)
catch_discover_tests(engine_tests)
