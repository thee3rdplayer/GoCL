cmake_minimum_required(VERSION 3.20)
project(GoCL VERSION 0.1.0 LANGUAGES CXX)

if(WIN32)
    set(GOCL_PROXY_LIBRARY "vulkan_proxy.dll")
else()
    set(GOCL_PROXY_LIBRARY "vulkan_proxy.so")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(GOCL_ENABLE_TESTS      "Build test executables"               OFF)
option(GOCL_FORCE_SWIFTSHADER "Force SwiftShader/Lavapipe ICD"       OFF)
option(GOCL_ENABLE_LAYER      "Build Vulkan proxy library for pre‑built games" OFF)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
include(FetchContent)

# stb – single-header image loader.
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        master)
FetchContent_MakeAvailable(stb)

# ---------------------------------------------------------------------------
# basisu – universal texture transcoder (ETC2 fallback)
# ---------------------------------------------------------------------------
FetchContent_Declare(basisu
    GIT_REPOSITORY https://github.com/BinomialLLC/basis_universal.git
    GIT_TAG        master)
set(BASISU_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BASISU_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(BASISU_BUILD_TOOLS    OFF CACHE BOOL "" FORCE)
set(BASISU_OPENCL         OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(basisu)

# ---------------------------------------------------------------------------
# ASTC encoder – AVX2 on x86‑64 hardware
# ---------------------------------------------------------------------------
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    set(ASTCENC_ISA_AVX2 ON CACHE BOOL "" FORCE)
endif()

set(ASTCENC_ISA_SSE41       OFF CACHE BOOL "" FORCE)
set(ASTCENC_ISA_SSE2        OFF CACHE BOOL "" FORCE)
set(ASTCENC_ISA_NEON        OFF CACHE BOOL "" FORCE)
set(ASTCENC_UNIVERSAL_BUILD OFF CACHE BOOL "" FORCE)
set(ASTCENC_CLI             OFF CACHE BOOL "" FORCE)
set(ASTCENC_UNITTEST        OFF CACHE BOOL "" FORCE)
FetchContent_Declare(astcenc
    GIT_REPOSITORY https://github.com/ARM-software/astc-encoder.git
    GIT_TAG        main)
FetchContent_MakeAvailable(astcenc)

find_package(Vulkan REQUIRED)

# GLM – header-only math library.
FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.1)
set(GLM_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glm)

# ---------------------------------------------------------------------------
# GoCL core library – everything except texture compression
# ---------------------------------------------------------------------------
file(GLOB_RECURSE GOCL_ALL_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/*.cpp")
list(REMOVE_ITEM GOCL_ALL_SOURCES
    "${CMAKE_SOURCE_DIR}/src/render/TextureManager.cpp"
    "${CMAKE_SOURCE_DIR}/src/render/ETC2Transcode.cpp"
)

add_library(GoCL_core STATIC ${GOCL_ALL_SOURCES})

target_include_directories(GoCL_core PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${stb_SOURCE_DIR}
    ${astcenc_SOURCE_DIR}/Source
)

target_link_libraries(GoCL_core PUBLIC
    Vulkan::Vulkan
    astcenc-avx2-static
    glm
)

target_compile_definitions(GoCL_core PRIVATE GOCL_VERSION_STRING="${PROJECT_VERSION}")

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(
        GoCL_core PRIVATE
        -Wno-deprecated-enum-enum-conversion
    )
endif()

# ---------------------------------------------------------------------------
# Texture compression helpers (used by engine and the on‑demand transcoder)
# ---------------------------------------------------------------------------
# Object library – compile once, share between static and shared libs
add_library(gocl_texture_obj OBJECT src/render/ETC2Transcode.cpp)

target_include_directories(gocl_texture_obj PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${stb_SOURCE_DIR}
    ${astcenc_SOURCE_DIR}/Source
    ${basisu_SOURCE_DIR}/transcoder
    ${basisu_SOURCE_DIR}/encoder
)

target_link_libraries(gocl_texture_obj PUBLIC
    Vulkan::Vulkan
    astcenc-avx2-static
    glm::glm
)

# Static texture library (always needed)
add_library(GoCL_texture STATIC
    src/render/TextureManager.cpp
    $<TARGET_OBJECTS:gocl_texture_obj>
)

target_include_directories(GoCL_texture PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${stb_SOURCE_DIR}
    ${astcenc_SOURCE_DIR}/Source
    ${basisu_SOURCE_DIR}/transcoder
    ${basisu_SOURCE_DIR}/encoder
)

target_link_libraries(GoCL_texture PUBLIC
    Vulkan::Vulkan
    astcenc-avx2-static
    glm::glm
)

set_target_properties(GoCL_core GoCL_texture PROPERTIES PREFIX "")

# Shared transcoder library – loaded on demand by the proxy
if(GOCL_ENABLE_LAYER)
    add_library(gocl_transcoder SHARED
        $<TARGET_OBJECTS:gocl_texture_obj>
    )
    target_link_libraries(gocl_transcoder PRIVATE basisu_encoder)
    set_target_properties(gocl_transcoder PROPERTIES
        PREFIX ""
        OUTPUT_NAME gocl_transcoder
    )
endif()

# ---------------------------------------------------------------------------
# Aggregate public GoCL target.
# ---------------------------------------------------------------------------
add_library(GoCL INTERFACE)
target_link_libraries(GoCL INTERFACE GoCL_core GoCL_texture)
target_include_directories(GoCL INTERFACE ${CMAKE_SOURCE_DIR}/src) 

# ---------------------------------------------------------------------------
# Install static libraries (for linking against the engine)
# ---------------------------------------------------------------------------
install(TARGETS GoCL_core GoCL_texture ARCHIVE DESTINATION .) 

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(GOCL_ENABLE_TESTS)
    enable_testing()
    set(TEST_DIR ${CMAKE_SOURCE_DIR}/tests)

    # Auto‑discover all C++ files ending with "Test.cpp" (e.g., *Test.cpp)
    file(GLOB TEST_SOURCES "${TEST_DIR}/*Test.cpp")

    foreach(TEST_SOURCE ${TEST_SOURCES})
        # Extract base name 
        get_filename_component(TEST_NAME ${TEST_SOURCE} NAME_WE)

        add_executable(${TEST_NAME} ${TEST_SOURCE})
        target_link_libraries(${TEST_NAME} PRIVATE GoCL Vulkan::Vulkan)
        target_include_directories(${TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/src)
        add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})

        if(GOCL_FORCE_SWIFTSHADER)
            set_tests_properties(${TEST_NAME} PROPERTIES
                ENVIRONMENT
                "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json")
        endif()
    endforeach()
endif() # GOCL_ENABLE_TESTS

# ---------------------------------------------------------------------------
# Vulkan proxy layer – implicit layer that injects GoCL optimisations
# into any Vulkan application.  The JSON manifest is copied alongside
# the library so the Vulkan loader can discover it.
# ---------------------------------------------------------------------------
if(GOCL_ENABLE_LAYER)
    add_library(vulkan_proxy SHARED src/layer/Proxy.cpp)

    configure_file(
        ${CMAKE_SOURCE_DIR}/src/layer/VkLayer_GoCL_Proxy.json.in
        ${CMAKE_BINARY_DIR}/VkLayer_GoCL_Proxy.json
        @ONLY
    )
    target_link_libraries(vulkan_proxy PRIVATE
        GoCL_core
        Vulkan::Vulkan
        ${CMAKE_DL_LIBS}
    )
    target_compile_definitions(vulkan_proxy PRIVATE GOCL_VERSION_STRING="${PROJECT_VERSION}") 
    target_include_directories(vulkan_proxy PRIVATE ${CMAKE_SOURCE_DIR}/src)
    set_target_properties(vulkan_proxy PROPERTIES
        PREFIX ""
        OUTPUT_NAME "vulkan_proxy"
    )

    if(UNIX OR ANDROID)
        target_link_options(vulkan_proxy PRIVATE "-Wl,-z,now")
    endif()

    # ---------------------------------------------------------------
    # Install rules – only the proxy, transcoder, and layer manifest
    # ---------------------------------------------------------------
    install(TARGETS vulkan_proxy
        RUNTIME DESTINATION .   # Windows .dll
        LIBRARY DESTINATION .   # Linux .so
    )
    install(TARGETS gocl_transcoder
        RUNTIME DESTINATION .
        LIBRARY DESTINATION .
    )
    install(FILES ${CMAKE_BINARY_DIR}/VkLayer_GoCL_Proxy.json DESTINATION .) 
endif()