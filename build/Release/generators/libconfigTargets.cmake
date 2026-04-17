# Load the debug and release variables
file(GLOB DATA_FILES "${CMAKE_CURRENT_LIST_DIR}/libconfig-*-data.cmake")

foreach(f ${DATA_FILES})
    include(${f})
endforeach()

# Create the targets for all the components
foreach(_COMPONENT ${libconfig_COMPONENT_NAMES} )
    if(NOT TARGET ${_COMPONENT})
        add_library(${_COMPONENT} INTERFACE IMPORTED)
        message(${libconfig_MESSAGE_MODE} "Conan: Component target declared '${_COMPONENT}'")
    endif()
endforeach()

if(NOT TARGET libconfig::libconfig)
    add_library(libconfig::libconfig INTERFACE IMPORTED)
    message(${libconfig_MESSAGE_MODE} "Conan: Target declared 'libconfig::libconfig'")
endif()
# Load the debug and release library finders
file(GLOB CONFIG_FILES "${CMAKE_CURRENT_LIST_DIR}/libconfig-Target-*.cmake")

foreach(f ${CONFIG_FILES})
    include(${f})
endforeach()