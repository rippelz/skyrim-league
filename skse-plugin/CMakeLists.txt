set(COMMONLIB_ROOT "${PROJECT_SOURCE_DIR}/.deps/CommonLibSSE-NG-current" CACHE PATH "CommonLibSSE-NG v7.1.0 root")
if(NOT EXISTS "${COMMONLIB_ROOT}/CMakeLists.txt")
  message(FATAL_ERROR "Run tools/fetch_deps.py or set COMMONLIB_ROOT")
endif()
set(BRIDGE_COMMONLIB_PREBUILT "" CACHE PATH "Verified upstream CommonLib static-library bundle (optional)")
if(BRIDGE_COMMONLIB_PREBUILT)
  find_package(spdlog CONFIG REQUIRED)
  if(NOT EXISTS "${BRIDGE_COMMONLIB_PREBUILT}/lib/CommonLibSSE.lib")
    message(FATAL_ERROR "Incomplete CommonLib bundle")
  endif()
  # The published bundle bakes all three runtime layouts and /MD. Consumers must
  # use exactly those layout macros even though this plugin rejects Skyrim VR.
  add_library(CommonLibSSE::CommonLibSSE STATIC IMPORTED GLOBAL)
  set_target_properties(CommonLibSSE::CommonLibSSE PROPERTIES
    IMPORTED_LOCATION "${BRIDGE_COMMONLIB_PREBUILT}/lib/CommonLibSSE.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${BRIDGE_COMMONLIB_PREBUILT}/include;${BRIDGE_COMMONLIB_PREBUILT}/extern/openvr/headers"
    INTERFACE_COMPILE_DEFINITIONS "ENABLE_SKYRIM_SE=1;ENABLE_SKYRIM_AE=1;ENABLE_SKYRIM_VR=1;HAS_SKYRIM_MULTI_TARGETING=1;WINVER=0x0601;_WIN32_WINNT=0x0601"
    INTERFACE_COMPILE_FEATURES cxx_std_23
    INTERFACE_LINK_LIBRARIES "spdlog::spdlog;${BRIDGE_COMMONLIB_PREBUILT}/lib/openvr_api.lib;advapi32.lib;bcrypt.lib;d3d11.lib;d3dcompiler.lib;dbghelp.lib;dxgi.lib;ole32.lib;version.lib")
else()
  set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(ENABLE_SKYRIM_VR OFF CACHE BOOL "" FORCE)
  set(SKSE_SUPPORT_XBYAK OFF CACHE BOOL "" FORCE)
  add_subdirectory("${COMMONLIB_ROOT}" commonlib EXCLUDE_FROM_ALL)
endif()
include("${COMMONLIB_ROOT}/cmake/CommonLibSSE.cmake")
add_commonlibsse_plugin(SkyrimRocketBridge NAME "SkyrimRocketBridge" AUTHOR "RocketSkyrimBridge contributors"
  USE_ADDRESS_LIBRARY SOURCES src/main.cpp src/DirectRender.cpp src/TerrainExport.cpp src/CarMenu.cpp)
target_link_libraries(SkyrimRocketBridge PRIVATE bridge_shared)
target_compile_definitions(SkyrimRocketBridge PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(SkyrimRocketBridge PRIVATE /EHsc /W4 /Zc:preprocessor /utf-8)
target_precompile_headers(SkyrimRocketBridge PRIVATE src/PCH.hpp)
