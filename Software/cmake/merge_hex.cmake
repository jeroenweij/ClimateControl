# Concatenate several non-overlapping Intel HEX images into one file.
#
#   cmake -DHEX_OUT=<out.hex> -DHEX_IN="<a.hex>;<b.hex>;..." -P merge_hex.cmake
#
# List the images in ascending flash-address order (bootloader first). The
# result keeps the first image's start-address record (type 03/05) and a single
# trailing end-of-file record (type 01); every other type-03/05 and every
# intermediate EOF is dropped. Inputs must not overlap -- no address checking is
# done here. Output is written with CRLF line endings, matching GNU objcopy.
#
# Used by add_stm32_executable(... BOOTLOADER <tgt>) in stm32.cmake to emit
# <name>-full.hex for one-shot factory programming.

if(NOT HEX_OUT OR NOT HEX_IN)
    message(FATAL_ERROR "merge_hex: HEX_OUT and HEX_IN are required")
endif()

list(LENGTH HEX_IN _count)
math(EXPR _lastIndex "${_count} - 1")

set(_out "")
set(_index 0)
foreach(_file IN LISTS HEX_IN)
    if(NOT EXISTS "${_file}")
        message(FATAL_ERROR "merge_hex: input not found: ${_file}")
    endif()

    # Read the whole file and split on either line ending -- objcopy emits CRLF.
    file(READ "${_file}" _content)
    string(REGEX REPLACE "\r?\n" ";" _content "${_content}")

    foreach(_line IN LISTS _content)
        string(STRIP "${_line}" _line)
        if(NOT _line MATCHES "^:[0-9A-Fa-f][0-9A-Fa-f]")
            continue() # blank line or trailing fragment
        endif()

        string(SUBSTRING "${_line}" 7 2 _rtype) # record type nibble pair
        if(_rtype STREQUAL "01" AND NOT _index EQUAL _lastIndex)
            continue() # keep EOF only from the last image
        endif()
        if((_rtype STREQUAL "03" OR _rtype STREQUAL "05") AND NOT _index EQUAL 0)
            continue() # keep the start-address record only from the first image
        endif()

        string(APPEND _out "${_line}\r\n")
    endforeach()

    math(EXPR _index "${_index} + 1")
endforeach()

if(_out STREQUAL "")
    message(FATAL_ERROR "merge_hex: produced an empty image from: ${HEX_IN}")
endif()

file(WRITE "${HEX_OUT}" "${_out}")
message(STATUS "merge_hex: wrote ${HEX_OUT} (${_count} images)")
