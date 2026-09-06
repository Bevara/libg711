cmake_minimum_required(VERSION 3.7)
project(Filters)

include(filters.cmake)

add_definitions(-fpic)

SET(LIBG711_SRC
        ${CMAKE_CURRENT_SOURCE_DIR}/dec_g711.c
        ${CMAKE_CURRENT_SOURCE_DIR}/reframe_g711.c
        ${CMAKE_CURRENT_SOURCE_DIR}/dec_au.c
)

# G.711 itself needs no library - two companding laws, expanded from their
# definition in the standard inside the filter. The G.721/G.723 ADPCM members
# of the same family do: libg72x.a is Sun's public-domain reference
# implementation, as carried in libsndfile's src/G72x with the block wrapper
# that handles the per-rate bit packing.
SET(LIBG711_LIBS
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/libg72x.a
)

SET(LIBG711_INC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_filter(libg711
        "${LIBG711_SRC}"
        "${LIBG711_LIBS}"
        []
        ""
        "${LIBG711_INC}"
        ""
        "1")
