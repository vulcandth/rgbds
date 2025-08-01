# SPDX-License-Identifier: MIT

configure_file(version.cpp _version.cpp ESCAPE_QUOTES)

set(common_src
    "extern/getopt.cpp"
    "diagnostics.cpp"
    "usage.cpp"
    "_version.cpp"
    )

find_package(BISON 3.0.0 REQUIRED)
set(BISON_FLAGS "-Wall -Dlr.type=ielr")
# Set some optimization flags on versions that support them
if(BISON_VERSION VERSION_GREATER_EQUAL "3.5")
  set(BISON_FLAGS "${BISON_FLAGS} -Dparse.lac=full -Dapi.token.raw=true")
endif()
if(BISON_VERSION VERSION_GREATER_EQUAL "3.6")
  set(BISON_FLAGS "${BISON_FLAGS} -Dparse.error=detailed")
else()
  set(BISON_FLAGS "${BISON_FLAGS} -Dparse.error=verbose")
endif()

BISON_TARGET(ASM_PARSER "asm/parser.y"
             "${PROJECT_SOURCE_DIR}/src/asm/parser.cpp"
             COMPILE_FLAGS "${BISON_FLAGS}"
             DEFINES_FILE "${PROJECT_SOURCE_DIR}/src/asm/parser.hpp"
             )

BISON_TARGET(LINKER_SCRIPT_PARSER "link/script.y"
             "${PROJECT_SOURCE_DIR}/src/link/script.cpp"
             COMPILE_FLAGS "${BISON_FLAGS}"
             DEFINES_FILE "${PROJECT_SOURCE_DIR}/src/link/script.hpp"
             )

set(rgbasm_src
    "${BISON_ASM_PARSER_OUTPUT_SOURCE}"
    "asm/actions.cpp"
    "asm/charmap.cpp"
    "asm/fixpoint.cpp"
    "asm/format.cpp"
    "asm/fstack.cpp"
    "asm/lexer.cpp"
    "asm/macro.cpp"
    "asm/main.cpp"
    "asm/opt.cpp"
    "asm/output.cpp"
    "asm/rpn.cpp"
    "asm/section.cpp"
    "asm/symbol.cpp"
    "asm/warning.cpp"
    "extern/utf8decoder.cpp"
    "linkdefs.cpp"
    "opmath.cpp"
    "util.cpp"
    )

set(rgblink_src
    "${BISON_LINKER_SCRIPT_PARSER_OUTPUT_SOURCE}"
    "link/assign.cpp"
    "link/lexer.cpp"
    "link/layout.cpp"
    "link/main.cpp"
    "link/object.cpp"
    "link/output.cpp"
    "link/patch.cpp"
    "link/sdas_obj.cpp"
    "link/section.cpp"
    "link/symbol.cpp"
    "link/warning.cpp"
    "extern/utf8decoder.cpp"
    "linkdefs.cpp"
    "opmath.cpp"
    "util.cpp"
    )

set(rgbfix_src
    "fix/main.cpp"
    "fix/mbc.cpp"
    "fix/warning.cpp"
    )

set(rgbgfx_src
    "gfx/color_set.cpp"
    "gfx/main.cpp"
    "gfx/pal_packing.cpp"
    "gfx/pal_sorting.cpp"
    "gfx/pal_spec.cpp"
    "gfx/png.cpp"
    "gfx/process.cpp"
    "gfx/reverse.cpp"
    "gfx/rgba.cpp"
    "gfx/warning.cpp"
    "util.cpp"
    )

foreach(PROG "asm" "fix" "gfx" "link")
  add_executable(rgb${PROG}
                 ${rgb${PROG}_src}
                 ${common_src}
                 )
  install(TARGETS rgb${PROG} RUNTIME DESTINATION bin)
  # Required to run tests
  set_target_properties(rgb${PROG} PROPERTIES
  # hack for MSVC: no-op generator expression to stop generation of "per-configuration subdirectory"
                        RUNTIME_OUTPUT_DIRECTORY $<1:${CMAKE_SOURCE_DIR}>)
endforeach()

if(LIBPNG_FOUND) # pkg-config
  target_include_directories(rgbgfx PRIVATE ${LIBPNG_INCLUDE_DIRS})
  target_link_directories(rgbgfx PRIVATE ${LIBPNG_LIBRARY_DIRS})
  target_link_libraries(rgbgfx PRIVATE ${LIBPNG_LIBRARIES})
else()
  target_compile_definitions(rgbgfx PRIVATE ${PNG_DEFINITIONS})
  target_include_directories(rgbgfx PRIVATE ${PNG_INCLUDE_DIRS})
  target_link_libraries(rgbgfx PRIVATE ${PNG_LIBRARIES})
endif()

include(CheckLibraryExists)
check_library_exists("m" "sin" "" HAS_LIBM)
if(HAS_LIBM)
  target_link_libraries(rgbasm PRIVATE "m")
endif()
