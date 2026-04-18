if (DESKTOP_APP_PRINT_LINK_FLAGS)
    include(CMakePrintHelpers)

    foreach(_ptg_diag_var
        CMAKE_INTERPROCEDURAL_OPTIMIZATION
        DESKTOP_APP_ENABLE_LTO
        DESKTOP_APP_SPECIAL_TARGET
        CMAKE_C_COMPILE_OPTIONS_IPO
        CMAKE_CXX_COMPILE_OPTIONS_IPO
        CMAKE_EXE_LINKER_FLAGS_IPO
        CMAKE_AR
        CMAKE_RANLIB
        CMAKE_NM
        CMAKE_LINKER
    )
        if(NOT DEFINED ${_ptg_diag_var})
            set(${_ptg_diag_var} "")
        endif()
    endforeach()
    unset(_ptg_diag_var)

    message(STATUS "")
    message(STATUS "=== LINKER FLAG DIAGNOSTIC ===")
    message(STATUS "")
    message(STATUS "-- Environment --")
    message(STATUS "  ENV{LDFLAGS}                    = $ENV{LDFLAGS}")
    message(STATUS "  ENV{CFLAGS}                     = $ENV{CFLAGS}")
    message(STATUS "  ENV{CXXFLAGS}                   = $ENV{CXXFLAGS}")
    message(STATUS "")
    message(STATUS "-- CMake linker flag variables --")
    message(STATUS "  CMAKE_EXE_LINKER_FLAGS          = ${CMAKE_EXE_LINKER_FLAGS}")
    message(STATUS "  CMAKE_EXE_LINKER_FLAGS_DEBUG    = ${CMAKE_EXE_LINKER_FLAGS_DEBUG}")
    message(STATUS "  CMAKE_EXE_LINKER_FLAGS_RELEASE  = ${CMAKE_EXE_LINKER_FLAGS_RELEASE}")
    message(STATUS "  CMAKE_SHARED_LINKER_FLAGS       = ${CMAKE_SHARED_LINKER_FLAGS}")
    message(STATUS "  CMAKE_MODULE_LINKER_FLAGS       = ${CMAKE_MODULE_LINKER_FLAGS}")
    message(STATUS "")
    message(STATUS "-- CMake compiler flag variables --")
    message(STATUS "  CMAKE_C_FLAGS                   = ${CMAKE_C_FLAGS}")
    message(STATUS "  CMAKE_C_FLAGS_DEBUG             = ${CMAKE_C_FLAGS_DEBUG}")
    message(STATUS "  CMAKE_C_FLAGS_RELEASE           = ${CMAKE_C_FLAGS_RELEASE}")
    message(STATUS "  CMAKE_CXX_FLAGS                 = ${CMAKE_CXX_FLAGS}")
    message(STATUS "  CMAKE_CXX_FLAGS_DEBUG           = ${CMAKE_CXX_FLAGS_DEBUG}")
    message(STATUS "  CMAKE_CXX_FLAGS_RELEASE         = ${CMAKE_CXX_FLAGS_RELEASE}")
    message(STATUS "")
    message(STATUS "-- IPO / LTO settings --")
    message(STATUS "  CMAKE_INTERPROCEDURAL_OPTIMIZATION         = ${CMAKE_INTERPROCEDURAL_OPTIMIZATION}")
    message(STATUS "  DESKTOP_APP_ENABLE_LTO                     = ${DESKTOP_APP_ENABLE_LTO}")
    message(STATUS "  DESKTOP_APP_SPECIAL_TARGET                 = ${DESKTOP_APP_SPECIAL_TARGET}")
    message(STATUS "  CMAKE_C_COMPILE_OPTIONS_IPO                = ${CMAKE_C_COMPILE_OPTIONS_IPO}")
    message(STATUS "  CMAKE_CXX_COMPILE_OPTIONS_IPO              = ${CMAKE_CXX_COMPILE_OPTIONS_IPO}")
    message(STATUS "  CMAKE_EXE_LINKER_FLAGS_IPO                 = ${CMAKE_EXE_LINKER_FLAGS_IPO}")
    message(STATUS "  CMAKE_AR                                   = ${CMAKE_AR}")
    message(STATUS "  CMAKE_RANLIB                               = ${CMAKE_RANLIB}")
    message(STATUS "  CMAKE_NM                                   = ${CMAKE_NM}")
    message(STATUS "")
    message(STATUS "-- common_options target properties (may contain generator expressions) --")
    cmake_print_properties(TARGETS common_options PROPERTIES
        INTERFACE_COMPILE_OPTIONS
        INTERFACE_COMPILE_DEFINITIONS
        INTERFACE_LINK_OPTIONS
        INTERFACE_LINK_LIBRARIES
    )
    message(STATUS "")
    message(STATUS "-- Toolchain --")
    message(STATUS "  CMAKE_C_COMPILER                = ${CMAKE_C_COMPILER}")
    message(STATUS "  CMAKE_CXX_COMPILER              = ${CMAKE_CXX_COMPILER}")
    message(STATUS "  CMAKE_LINKER                    = ${CMAKE_LINKER}")
    message(STATUS "  CMAKE_C_COMPILER_VERSION        = ${CMAKE_C_COMPILER_VERSION}")
    message(STATUS "  CMAKE_CXX_COMPILER_VERSION      = ${CMAKE_CXX_COMPILER_VERSION}")
    message(STATUS "")
    message(STATUS "NOTE: To see the fully resolved link command at build time, run:")
    message(STATUS "  VERBOSE=1 cmake --build out --config <Debug|Release> --target Telegram")
    message(STATUS "  (or pass -DCMAKE_VERBOSE_MAKEFILE=ON at configure time)")
    message(STATUS "=== END LINKER FLAG DIAGNOSTIC ===")
    message(STATUS "")
endif()
