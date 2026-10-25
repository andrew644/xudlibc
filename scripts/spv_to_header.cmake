# spv_to_header.cmake - turn built SPIR-V into C arrays of uint32_t words.
#
#   cmake -DSPV_OUT_HEADER=<out.h> -DSPV_GUARD=<GUARD> -DSPV_COUNT=<n> \
#         -DSPV_ARRAY_<i>=<name> -DSPV_INPUT_<i>=<in.spv>  (i = 0..n-1) \
#         -P scripts/spv_to_header.cmake
#
# The array names and the include guard come from the caller, so CMakeLists.txt owns
# the spelling and this script owns only the byte-to-word step. SPIR-V words are 32-bit
# and in the host's native order (the SPIR-V spec's own rule), so od's -tu4 (native
# unsigned int) is exactly the step VkShaderModuleCreateInfo::pCode wants: the arrays
# below are words, not bytes, and nothing past here bakes in a byte order.
#
# -D variables rather than positional arguments, and ONE variable per input rather than
# a list: cmake 4.4.4 here leaves both ARGS and ARGV empty for `cmake -P script.cmake one
# two three` (probed), so positional script arguments are not available on this toolchain
# and -D values are. Likewise no string(UPCASE) and no string(SPLIT) (both probed here:
# "string does not recognize sub-command UPPERCASE"/"SPLIT"), which is why the guard is
# passed in rather than derived. A list passed as one -D value cannot be recovered either:
# inside add_custom_command the escaped semicolon either survives VERBATIM as a literal
# backslash-semicolon (one two-path filename) or, without VERBATIM, reaches the shell
# unquoted and splits the command line. Indexed -D names are the one form that is safe
# under both.

if(NOT SPV_OUT_HEADER OR NOT SPV_GUARD OR NOT SPV_COUNT)
    message(FATAL_ERROR "SPV_OUT_HEADER, SPV_GUARD and SPV_COUNT are required")
endif()

set(SPV_BODY "")
set(SPV_INDEX 0)
while(SPV_INDEX LESS SPV_COUNT)
    set(SPV_NAME_VAR "SPV_ARRAY_${SPV_INDEX}")
    set(SPV_FILE_VAR "SPV_INPUT_${SPV_INDEX}")
    if(NOT ${SPV_NAME_VAR} OR NOT ${SPV_FILE_VAR})
        message(FATAL_ERROR "SPV_ARRAY_${SPV_INDEX} and SPV_INPUT_${SPV_INDEX} are required")
    endif()
    set(SPV_ARRAY "${${SPV_NAME_VAR}}")
    set(SPV_FILE "${${SPV_FILE_VAR}}")

    execute_process(
        COMMAND od -An -v -tu4 "${SPV_FILE}"
        OUTPUT_VARIABLE SPV_OD_OUT
        RESULT_VARIABLE SPV_OD_RC)
    if(NOT SPV_OD_RC EQUAL 0)
        message(FATAL_ERROR "od failed on ${SPV_FILE}")
    endif()

    string(REGEX REPLACE "[ \t\r\n]+" ";" SPV_WORDS "${SPV_OD_OUT}")

    set(SPV_TEXT "")
    set(SPV_WORD_COUNT 0)
    foreach(SPV_WORD IN ITEMS ${SPV_WORDS})
        if(SPV_WORD MATCHES "[0-9]")
            string(APPEND SPV_TEXT "${SPV_WORD}u,")
            math(EXPR SPV_WORD_COUNT "${SPV_WORD_COUNT} + 1")
        endif()
    endforeach()
    string(REGEX REPLACE ",$" "" SPV_TEXT "${SPV_TEXT}")
    if(SPV_WORD_COUNT EQUAL 0)
        message(FATAL_ERROR "no words read from ${SPV_FILE}")
    endif()

    string(APPEND SPV_BODY "/* ${SPV_FILE} - ${SPV_WORD_COUNT} words, built by glslangValidator, validated by spirv-val */\nstatic const uint32_t ${SPV_ARRAY}[] = {\n    ${SPV_TEXT}\n};\n\n")

    math(EXPR SPV_INDEX "${SPV_INDEX} + 1")
endwhile()

file(WRITE "${SPV_OUT_HEADER}" "/***********************************************************************************************
 *   GENERATED FILE - do not edit. Built by CMakeLists.txt from the shaders .vert and
 *                    .frag sources with glslangValidator, then validated with
 *                    spirv-val, then turned into words by this script. The sources
 *                    live in shaders/ and are committed; this lands in the build
 *                    tree and is not, and neither is the .spv itself.
 ***********************************************************************************************/

#ifndef ${SPV_GUARD}_
#define ${SPV_GUARD}_

/* One array per shader, as the uint32_t words VkShaderModuleCreateInfo::pCode wants.
   Sizes are taken with sizeof in C, so nothing here duplicates a word count. */

${SPV_BODY}

#endif /* ${SPV_GUARD}_ */
")
