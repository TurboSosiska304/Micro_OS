# Micro OS — the part of a build that makes it reproducible.
#
# SOURCE_DATE_EPOCH is the convention of reproducible builds: one moment,
# given by the caller, instead of the clock of the machine that compiles.
# The kernel id of components/kernel/src/version.c is __DATE__ and __TIME__,
# and a path of the build directory reaches the debug information and the
# __FILE__ of the panic message; both would make two builds of the same tree
# differ. With SOURCE_DATE_EPOCH the id is fixed and the paths are relative
# to the source tree; without it the build stays what it was, a build of
# "now", and the message says so.

if(DEFINED ENV{SOURCE_DATE_EPOCH})
    # CMake can format the current moment, not an epoch. The formatter of
    # Python is already a requirement of the tools, and %b %e %Y is the
    # shape the C compiler's __DATE__ has.
    string(CONCAT _source_date_python
        "import datetime, sys; "
        "t = datetime.datetime.fromtimestamp(int(sys.argv[1]), datetime.timezone.utc); "
        "print(t.strftime('%b %e %Y')); print(t.strftime('%H:%M:%S'))"
    )

    execute_process(
        COMMAND python3 -c "${_source_date_python}" "$ENV{SOURCE_DATE_EPOCH}"
        OUTPUT_VARIABLE _source_date
        ERROR_VARIABLE _source_date_error
        RESULT_VARIABLE _source_date_status
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    if(NOT _source_date_status EQUAL 0)
        message(FATAL_ERROR "Micro OS: SOURCE_DATE_EPOCH is not a number: ${_source_date_error}")
    endif()

    string(REPLACE "\n" ";" _source_date "${_source_date}")
    list(GET _source_date 0 _source_day)
    list(GET _source_date 1 _source_time)

    message(STATUS "Micro OS: a reproducible build at ${_source_day} ${_source_time} UTC")

    add_compile_options(
        "$<$<COMPILE_LANGUAGE:C>:-Wno-builtin-macro-redefined>"
        "$<$<COMPILE_LANGUAGE:C>:-D__DATE__=\"${_source_day}\">"
        "$<$<COMPILE_LANGUAGE:C>:-D__TIME__=\"${_source_time}\">"
    )
else()
    message(STATUS "Micro OS: SOURCE_DATE_EPOCH is not set, the build is not reproducible")
endif()

# The paths of the source tree leave the binaries as relative ones, so a
# debugger of the machine can still find them and two checkouts in two
# directories give the same bytes. The root is taken from this file, not
# from the project: the x86-64 port is a CMake project of its own in a
# subdirectory of the same tree.
get_filename_component(_reproducible_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
add_compile_options(
    "$<$<COMPILE_LANGUAGE:C>:-ffile-prefix-map=${_reproducible_root}=.>"
)
