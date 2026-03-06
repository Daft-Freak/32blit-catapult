# Basic parameters; check that these match your project / environment
cmake_minimum_required(VERSION 3.15)

# Replace "game" with a name for your project (this is used the name of the output)
project(catapult)

# Add your sources here (adding headers is optional, but helps some CMake generators)
set(PROJECT_SOURCE game.cpp)

# ... and any other files you want in the release here
set(PROJECT_DISTRIBS LICENSE README.md)

# Build configuration; approach this with caution!
if(MSVC)
  add_compile_options("/W4" "/wd4244" "/wd4324" "/wd4458" "/wd4100")
else()
  add_compile_options("-Wall" "-Wextra" "-Wdouble-promotion" "-Wno-unused-parameter")

  if(NOT 32BLIT_HW AND NOT PICO_SDK_PATH)
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fsanitize=address")
  endif()
endif()

find_package (32BLIT CONFIG REQUIRED PATHS ../32blit-sdk $ENV{PATH_32BLIT_SDK})

add_subdirectory(${32BLIT_DIR}/launcher-shared launcher-shared)

add_subdirectory(DUH)

blit_executable (${PROJECT_NAME}
  PICO_BLIT PICO_BLIT_OFFSET_KB 256
  ${PROJECT_SOURCE}
)
blit_assets_yaml (${PROJECT_NAME} assets.yml)
blit_metadata (${PROJECT_NAME} metadata.yml)

target_link_libraries(${PROJECT_NAME} LauncherShared DUH)

add_custom_target (flash DEPENDS ${PROJECT_NAME}.flash)

# setup release packages
install (FILES ${PROJECT_DISTRIBS} DESTINATION .)
set (CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
set (CPACK_GENERATOR "ZIP" "TGZ")
include (CPack)
