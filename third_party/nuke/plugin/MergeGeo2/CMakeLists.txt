set(PXR_PACKAGE MergeGeo2)

# compiler flags needed for building.
#  silence deprecated warning from usd /pxr/base/tf/hashset.h: '-Wno-deprecated'
#  silence 'unused' warnings from boost: '-Wno-unused-local-typedefs'
if (UNIX)
    add_definitions(
        -Wno-cast-qual
        -Wno-extra
        -Wno-reorder
        -Wno-deprecated
        -Wno-unused-local-typedefs
    )
endif()

pxr_plugin(${PXR_PACKAGE}
    LIBRARIES
        Fuser
        #
        ${NUKE_DDIMAGE_LIBRARY}

    INCLUDE_DIRS
        ${NUKE_INCLUDE_DIRS}

    CPPFILES
        MergeGeo2.cpp
)

install(
    FILES
        Scene2.tcl
    DESTINATION
        ${PXR_INSTALL_SUBDIR}/plugin
)
