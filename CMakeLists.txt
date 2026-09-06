cmake_minimum_required(VERSION 3.7)
project(Filters)

include(filters.cmake)

add_definitions(-fpic)

SET(LIBG711_SRC
        ${CMAKE_CURRENT_SOURCE_DIR}/dec_g711.c
        ${CMAKE_CURRENT_SOURCE_DIR}/reframe_g711.c
)

# No third-party archive: G.711 is two small companding laws, expanded from
# their definition in the standard inside the filter.
SET(LIBG711_LIBS)

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
