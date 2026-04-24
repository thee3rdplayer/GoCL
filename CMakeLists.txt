cmake_minimum_required(VERSION 3.20)
project(GoCL CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(GOCL_ENABLE_TESTS "Build test executables" OFF)
option(GOCL_FORCE_SWIFTSHADER "Force SwiftShader/Lavapipe ICD in tests" OFF)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
include(FetchContent)

FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        master)
FetchContent_MakeAvailable(stb)

FetchContent_Declare(basisu
    GIT_REPOSITORY https://github.com/BinomialLLC/basis_universal.git
    GIT_TAG        master)
FetchContent_MakeAvailable(basisu)

# astc-encoder — AVX2 on x86-64 desktop, fallback to SSE4.1
set(ASTCENC_ISA_AVX2     ON  CACHE BOOL "" FORCE)
set(ASTCENC_ISA_SSE41    OFF CACHE BOOL "" FORCE)
set(ASTCENC_ISA_SSE2     OFF CACHE BOOL "" FORCE)
set(ASTCENC_ISA_NEON     OFF CACHE BOOL "" FORCE)
set(ASTCENC_UNIVERSAL_BUILD OFF CACHE BOOL "" FORCE)
set(ASTCENC_CLI          OFF CACHE BOOL "" FORCE)
set(ASTCENC_UNITTEST     OFF CACHE BOOL "" FORCE)
FetchContent_Declare(astcenc
    GIT_REPOSITORY https://github.com/ARM-software/astc-encoder.git
    GIT_TAG        main)
FetchContent_MakeAvailable(astcenc)

find_package(Vulkan REQUIRED)

# ---------------------------------------------------------------------------
# GoCL static library
# ---------------------------------------------------------------------------
file(GLOB_RECURSE GOCL_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/core/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/render/*.cpp"
)

add_library(GoCL STATIC ${GOCL_SOURCES})

target_include_directories(GoCL PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${stb_SOURCE_DIR}
    ${basisu_SOURCE_DIR}
    ${basisu_SOURCE_DIR}/transcoder
    ${basisu_SOURCE_DIR}/encoder        
    ${astcenc_SOURCE_DIR}/Source
)

target_link_libraries(GoCL PUBLIC
    Vulkan::Vulkan
    basisu_encoder
    astcenc-avx2-static
)

# Suppress third-party enum arithmetic warning from basisu
target_compile_options(basisu_encoder PRIVATE -Wno-deprecated-enum-enum-conversion)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(GOCL_ENABLE_TESTS)
    enable_testing()

    # Shared test include path
    set(TEST_DIR ${CMAKE_SOURCE_DIR}/tests)

    foreach(TEST_NAME GoCLCapabilityTest GoCLTextureTest GoCLShaderTest GoCLContextTest)
        add_executable(${TEST_NAME} ${TEST_DIR}/${TEST_NAME}.cpp)
        target_link_libraries(${TEST_NAME} PRIVATE GoCL Vulkan::Vulkan)
        target_include_directories(${TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/src)
        add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
        if(GOCL_FORCE_SWIFTSHADER)
            set_tests_properties(${TEST_NAME} PROPERTIES
                ENVIRONMENT "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json")
        endif()
    endforeach()

    # GoCLShaderTest needs SPIR-V blobs — compile passthrough shaders if glslangValidator found
    find_program(GLSLANG glslangValidator)
    if(GLSLANG)
        file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/tests/data)
        add_custom_command(
            OUTPUT  ${CMAKE_BINARY_DIR}/tests/data/passthrough.vert.spv
                    ${CMAKE_BINARY_DIR}/tests/data/passthrough.frag.spv
            COMMAND ${GLSLANG} -V ${TEST_DIR}/data/passthrough.vert
                    -o ${CMAKE_BINARY_DIR}/tests/data/passthrough.vert.spv
            COMMAND ${GLSLANG} -V ${TEST_DIR}/data/passthrough.frag
                    -o ${CMAKE_BINARY_DIR}/tests/data/passthrough.frag.spv
            DEPENDS ${TEST_DIR}/data/passthrough.vert
                    ${TEST_DIR}/data/passthrough.frag
        )
        add_custom_target(GenerateTestSPIRV DEPENDS
            ${CMAKE_BINARY_DIR}/tests/data/passthrough.vert.spv
            ${CMAKE_BINARY_DIR}/tests/data/passthrough.frag.spv
        )
        add_dependencies(GoCLShaderTest GenerateTestSPIRV)
        target_compile_definitions(GoCLShaderTest PRIVATE
            GOCL_TEST_SPIRV_VERT="${CMAKE_BINARY_DIR}/tests/data/passthrough.vert.spv"
            GOCL_TEST_SPIRV_FRAG="${CMAKE_BINARY_DIR}/tests/data/passthrough.frag.spv"
        )
    else()
        message(WARNING "glslangValidator not found — GoCLShaderTest will fail at runtime")
    endif()
endif()