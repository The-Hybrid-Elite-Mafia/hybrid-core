# =============================================================================
#  ASIO (optional). OFF by default. When ON, the Steinberg ASIO SDK must already be fetched
#  (tools/fetch-asio-sdk.ps1) into external/asiosdk/sdk; configuration FAILS if it is missing so a
#  build can never quietly come out without ASIO.
#  The SDK is Steinberg's (GPLv3 or Steinberg proprietary). It is never committed.
# =============================================================================
option(HC_WITH_ASIO "Build the ASIO output (needs the Steinberg ASIO SDK, see tools/fetch-asio-sdk.ps1)" OFF)
set(HC_ASIO_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/asiosdk/sdk" CACHE PATH "Steinberg ASIO SDK folder")
set(HC_ASIO_SOURCES "")
set(HC_ASIO_INCLUDES "")
if(HC_WITH_ASIO)
  foreach(_f common/asio.h common/asio.cpp host/asiodrivers.cpp host/pc/asiolist.cpp)
    if(NOT EXISTS "${HC_ASIO_SDK_DIR}/${_f}")
      message(FATAL_ERROR "HC_WITH_ASIO=ON but ${HC_ASIO_SDK_DIR}/${_f} is missing. Run tools/fetch-asio-sdk.ps1 first.")
    endif()
  endforeach()
  set(HC_ASIO_SOURCES
      ${HC_ASIO_SDK_DIR}/common/asio.cpp
      ${HC_ASIO_SDK_DIR}/host/asiodrivers.cpp
      ${HC_ASIO_SDK_DIR}/host/pc/asiolist.cpp)
  set(HC_ASIO_INCLUDES ${HC_ASIO_SDK_DIR}/common ${HC_ASIO_SDK_DIR}/host ${HC_ASIO_SDK_DIR}/host/pc)
  message(STATUS "ASIO: ON (SDK at ${HC_ASIO_SDK_DIR})")
else()
  message(STATUS "ASIO: OFF (WASAPI only)")
endif()
