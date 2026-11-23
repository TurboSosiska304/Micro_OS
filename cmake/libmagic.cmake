# Micro OS — the sources of file 5.45 and libmagic.
#
# The same list serves the two builds that carry the port: the static archive
# of the guest (`ports/port_x86_64/CMakeLists.txt`) and the host test
# (`tests/CMakeLists.txt`). The list is here and not in one of the two so that
# a file added to the port is added to both, and the warnings of the vendored
# release are excused in one place.
#
# The upstream files are not edited where the port layer answers; the hunks
# that are unavoidable (the float of a magic type, the precision of the
# formatter, the `read` of the database) are named in
# `third_party/file/README.md`.

# The file is included by the root build and by the port of x86-64, whose
# CMAKE_SOURCE_DIR is ports/port_x86_64: the root of the tree is the parent of
# this directory and not the source directory of the project that includes it.
get_filename_component(MICROOS_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

set(MICROOS_FILE_DIR "${MICROOS_ROOT}/third_party/file")

set(MICROOS_FILE_SOURCES
    "${MICROOS_FILE_DIR}/src/apprentice.c"
    "${MICROOS_FILE_DIR}/src/ascmagic.c"
    "${MICROOS_FILE_DIR}/src/asctime_r.c"
    "${MICROOS_FILE_DIR}/src/buffer.c"
    "${MICROOS_FILE_DIR}/src/cdf.c"
    "${MICROOS_FILE_DIR}/src/cdf_time.c"
    "${MICROOS_FILE_DIR}/src/ctime_r.c"
    "${MICROOS_FILE_DIR}/src/der.c"
    "${MICROOS_FILE_DIR}/src/dprintf.c"
    "${MICROOS_FILE_DIR}/src/encoding.c"
    "${MICROOS_FILE_DIR}/src/fmtcheck.c"
    "${MICROOS_FILE_DIR}/src/fsmagic.c"
    "${MICROOS_FILE_DIR}/src/funcs.c"
    "${MICROOS_FILE_DIR}/src/gmtime_r.c"
    "${MICROOS_FILE_DIR}/src/is_csv.c"
    "${MICROOS_FILE_DIR}/src/is_json.c"
    "${MICROOS_FILE_DIR}/src/is_simh.c"
    "${MICROOS_FILE_DIR}/src/is_tar.c"
    "${MICROOS_FILE_DIR}/src/localtime_r.c"
    "${MICROOS_FILE_DIR}/src/magic.c"
    "${MICROOS_FILE_DIR}/src/pread.c"
    "${MICROOS_FILE_DIR}/src/print.c"
    "${MICROOS_FILE_DIR}/src/readcdf.c"
    "${MICROOS_FILE_DIR}/src/readelf.c"
    "${MICROOS_FILE_DIR}/src/softmagic.c"
    "${MICROOS_FILE_DIR}/src/strlcat.c"
    "${MICROOS_FILE_DIR}/src/strlcpy.c"
    "${MICROOS_FILE_DIR}/port/file_port.c"
)

# The release is somebody else's C: a warning of this tree is answered by an
# exception here and not by a hunk of the vendored file.
set(MICROOS_FILE_WARNINGS
    -Wno-sign-compare
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-format-nonliteral
    -Wno-cast-qual
    -Wno-maybe-uninitialized
    -Wno-missing-field-initializers
    -Wno-char-subscripts
    -Wno-undef
)

set(MICROOS_MAGIC_CONFIG "${MICROOS_FILE_DIR}/magic/subset.json")
set(MICROOS_MAGIC_SUBSET "${MICROOS_ROOT}/tools/magic_subset.py")
