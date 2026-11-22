# Micro OS — the application and its signature trailer fit the factory
# partition (ADR 0020).
#
# The trailer of the board is in the last 4 KiB of the app partition, and the
# app image is everything before it. A build that grows past the reserve would
# put the trailer over the image (or the image over the trailer), so the build
# says it here rather than on the board after a flash. Included by the root
# CMakeLists.txt of the ESP32 build; the paths and the numbers come from it.

if(NOT DEFINED IMAGE OR NOT DEFINED PARTITION_SIZE OR NOT DEFINED TRAILER_RESERVE)
    message(FATAL_ERROR "check_image_fits: IMAGE, PARTITION_SIZE and TRAILER_RESERVE are required")
endif()

if(NOT EXISTS "${IMAGE}")
    message(FATAL_ERROR "check_image_fits: ${IMAGE} is not there")
endif()

file(SIZE "${IMAGE}" image_size)
math(EXPR usable "${PARTITION_SIZE} - ${TRAILER_RESERVE}")

if(image_size GREATER usable)
    math(EXPR over "${image_size} - ${usable}")
    message(FATAL_ERROR
        "Micro OS: the image is ${image_size} bytes and the factory partition has "
        "${usable} bytes for it and its ${TRAILER_RESERVE}-byte trailer (${over} bytes over)")
endif()

message(STATUS
    "Micro OS: the image is ${image_size} bytes, the trailer reserve is ${TRAILER_RESERVE}, "
    "the partition has ${usable}")
