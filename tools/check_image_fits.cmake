# Micro OS — the application and its signature trailer fit an application
# slot (ADR 0020, ADR 0022), and leave the room a 1.x build needs to grow.
#
# The trailer of the board is in the last 4 KiB of the slot, and the app
# image is everything before it. A build that grows past the reserve would
# put the trailer over the image (or the image over the trailer), so the build
# says it here rather than on the board after a flash. MIN_FREE is the growth
# the compatibility port of ADR 0018 is promised: a build that comes within
# it is a decision about the layout and not a silent squeeze.
# Included by the root CMakeLists.txt of the ESP32 build; the paths and the
# numbers come from it.

if(NOT DEFINED IMAGE OR NOT DEFINED PARTITION_SIZE OR NOT DEFINED TRAILER_RESERVE)
    message(FATAL_ERROR "check_image_fits: IMAGE, PARTITION_SIZE and TRAILER_RESERVE are required")
endif()

if(NOT DEFINED MIN_FREE)
    set(MIN_FREE 0)
endif()

if(NOT EXISTS "${IMAGE}")
    message(FATAL_ERROR "check_image_fits: ${IMAGE} is not there")
endif()

file(SIZE "${IMAGE}" image_size)
math(EXPR usable "${PARTITION_SIZE} - ${TRAILER_RESERVE}")
math(EXPR free "${usable} - ${image_size}")

if(image_size GREATER usable)
    math(EXPR over "${image_size} - ${usable}")
    message(FATAL_ERROR
        "Micro OS: the image is ${image_size} bytes and the application slot has "
        "${usable} bytes for it and its ${TRAILER_RESERVE}-byte trailer (${over} bytes over)")
endif()

if(free LESS MIN_FREE)
    message(FATAL_ERROR
        "Micro OS: the image is ${image_size} bytes and leaves ${free} bytes of the "
        "${PARTITION_SIZE}-byte slot after its ${TRAILER_RESERVE}-byte trailer; the "
        "compatibility port keeps at least ${MIN_FREE} (ADR 0022)")
endif()

message(STATUS
    "Micro OS: the image is ${image_size} bytes, the trailer reserve is ${TRAILER_RESERVE}, "
    "the slot has ${usable} bytes and leaves ${free}")
