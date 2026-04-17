# Avoid multiple calls to find_package to append duplicated properties to the targets
include_guard()########### VARIABLES #######################################################################
#############################################################################################
set(libconfig_FRAMEWORKS_FOUND_RELEASE "") # Will be filled later
conan_find_apple_frameworks(libconfig_FRAMEWORKS_FOUND_RELEASE "${libconfig_FRAMEWORKS_RELEASE}" "${libconfig_FRAMEWORK_DIRS_RELEASE}")

set(libconfig_LIBRARIES_TARGETS "") # Will be filled later


######## Create an interface target to contain all the dependencies (frameworks, system and conan deps)
if(NOT TARGET libconfig_DEPS_TARGET)
    add_library(libconfig_DEPS_TARGET INTERFACE IMPORTED)
endif()

set_property(TARGET libconfig_DEPS_TARGET
             APPEND PROPERTY INTERFACE_LINK_LIBRARIES
             $<$<CONFIG:Release>:${libconfig_FRAMEWORKS_FOUND_RELEASE}>
             $<$<CONFIG:Release>:${libconfig_SYSTEM_LIBS_RELEASE}>
             $<$<CONFIG:Release>:libconfig::libconfig>)

####### Find the libraries declared in cpp_info.libs, create an IMPORTED target for each one and link the
####### libconfig_DEPS_TARGET to all of them
conan_package_library_targets("${libconfig_LIBS_RELEASE}"    # libraries
                              "${libconfig_LIB_DIRS_RELEASE}" # package_libdir
                              "${libconfig_BIN_DIRS_RELEASE}" # package_bindir
                              "${libconfig_LIBRARY_TYPE_RELEASE}"
                              "${libconfig_IS_HOST_WINDOWS_RELEASE}"
                              libconfig_DEPS_TARGET
                              libconfig_LIBRARIES_TARGETS  # out_libraries_targets
                              "_RELEASE"
                              "libconfig"    # package_name
                              "${libconfig_NO_SONAME_MODE_RELEASE}")  # soname

# FIXME: What is the result of this for multi-config? All configs adding themselves to path?
set(CMAKE_MODULE_PATH ${libconfig_BUILD_DIRS_RELEASE} ${CMAKE_MODULE_PATH})

########## COMPONENTS TARGET PROPERTIES Release ########################################

    ########## COMPONENT libconfig::libconfig++ #############

        set(libconfig_libconfig_libconfig++_FRAMEWORKS_FOUND_RELEASE "")
        conan_find_apple_frameworks(libconfig_libconfig_libconfig++_FRAMEWORKS_FOUND_RELEASE "${libconfig_libconfig_libconfig++_FRAMEWORKS_RELEASE}" "${libconfig_libconfig_libconfig++_FRAMEWORK_DIRS_RELEASE}")

        set(libconfig_libconfig_libconfig++_LIBRARIES_TARGETS "")

        ######## Create an interface target to contain all the dependencies (frameworks, system and conan deps)
        if(NOT TARGET libconfig_libconfig_libconfig++_DEPS_TARGET)
            add_library(libconfig_libconfig_libconfig++_DEPS_TARGET INTERFACE IMPORTED)
        endif()

        set_property(TARGET libconfig_libconfig_libconfig++_DEPS_TARGET
                     APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                     $<$<CONFIG:Release>:${libconfig_libconfig_libconfig++_FRAMEWORKS_FOUND_RELEASE}>
                     $<$<CONFIG:Release>:${libconfig_libconfig_libconfig++_SYSTEM_LIBS_RELEASE}>
                     $<$<CONFIG:Release>:${libconfig_libconfig_libconfig++_DEPENDENCIES_RELEASE}>
                     )

        ####### Find the libraries declared in cpp_info.component["xxx"].libs,
        ####### create an IMPORTED target for each one and link the 'libconfig_libconfig_libconfig++_DEPS_TARGET' to all of them
        conan_package_library_targets("${libconfig_libconfig_libconfig++_LIBS_RELEASE}"
                              "${libconfig_libconfig_libconfig++_LIB_DIRS_RELEASE}"
                              "${libconfig_libconfig_libconfig++_BIN_DIRS_RELEASE}" # package_bindir
                              "${libconfig_libconfig_libconfig++_LIBRARY_TYPE_RELEASE}"
                              "${libconfig_libconfig_libconfig++_IS_HOST_WINDOWS_RELEASE}"
                              libconfig_libconfig_libconfig++_DEPS_TARGET
                              libconfig_libconfig_libconfig++_LIBRARIES_TARGETS
                              "_RELEASE"
                              "libconfig_libconfig_libconfig++"
                              "${libconfig_libconfig_libconfig++_NO_SONAME_MODE_RELEASE}")


        ########## TARGET PROPERTIES #####################################
        set_property(TARGET libconfig::libconfig++
                     APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                     $<$<CONFIG:Release>:${libconfig_libconfig_libconfig++_OBJECTS_RELEASE}>
                     $<$<CONFIG:Release>:${libconfig_libconfig_libconfig++_LIBRARIES_TARGETS}>
                     )

        if("${libconfig_libconfig_libconfig++_LIBS_RELEASE}" STREQUAL "")
            # If the component is not declaring any "cpp_info.components['foo'].libs" the system, frameworks etc are not
            # linked to the imported targets and we need to do it to the global target
            set_property(TARGET libconfig::libconfig++
                         APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                         libconfig_libconfig_libconfig++_DEPS_TARGET)
        endif()

        set_property(TARGET libconfig::libconfig++ APPEND PROPERTY INTERFACE_LINK_OPTIONS
                     $<$<CONFIG:Release>:${libconfig_libconfig_libconfig++_LINKER_FLAGS_RELEASE}>)
        set_property(TARGET libconfig::libconfig++ APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES
                     $<$<CONFIG:Release>:${libconfig_libconfig_libconfig++_INCLUDE_DIRS_RELEASE}>)
        set_property(TARGET libconfig::libconfig++ APPEND PROPERTY INTERFACE_LINK_DIRECTORIES
                     $<$<CONFIG:Release>:${libconfig_libconfig_libconfig++_LIB_DIRS_RELEASE}>)
        set_property(TARGET libconfig::libconfig++ APPEND PROPERTY INTERFACE_COMPILE_DEFINITIONS
                     $<$<CONFIG:Release>:${libconfig_libconfig_libconfig++_COMPILE_DEFINITIONS_RELEASE}>)
        set_property(TARGET libconfig::libconfig++ APPEND PROPERTY INTERFACE_COMPILE_OPTIONS
                     $<$<CONFIG:Release>:${libconfig_libconfig_libconfig++_COMPILE_OPTIONS_RELEASE}>)


    ########## COMPONENT libconfig::libconfig #############

        set(libconfig_libconfig_libconfig_FRAMEWORKS_FOUND_RELEASE "")
        conan_find_apple_frameworks(libconfig_libconfig_libconfig_FRAMEWORKS_FOUND_RELEASE "${libconfig_libconfig_libconfig_FRAMEWORKS_RELEASE}" "${libconfig_libconfig_libconfig_FRAMEWORK_DIRS_RELEASE}")

        set(libconfig_libconfig_libconfig_LIBRARIES_TARGETS "")

        ######## Create an interface target to contain all the dependencies (frameworks, system and conan deps)
        if(NOT TARGET libconfig_libconfig_libconfig_DEPS_TARGET)
            add_library(libconfig_libconfig_libconfig_DEPS_TARGET INTERFACE IMPORTED)
        endif()

        set_property(TARGET libconfig_libconfig_libconfig_DEPS_TARGET
                     APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                     $<$<CONFIG:Release>:${libconfig_libconfig_libconfig_FRAMEWORKS_FOUND_RELEASE}>
                     $<$<CONFIG:Release>:${libconfig_libconfig_libconfig_SYSTEM_LIBS_RELEASE}>
                     $<$<CONFIG:Release>:${libconfig_libconfig_libconfig_DEPENDENCIES_RELEASE}>
                     )

        ####### Find the libraries declared in cpp_info.component["xxx"].libs,
        ####### create an IMPORTED target for each one and link the 'libconfig_libconfig_libconfig_DEPS_TARGET' to all of them
        conan_package_library_targets("${libconfig_libconfig_libconfig_LIBS_RELEASE}"
                              "${libconfig_libconfig_libconfig_LIB_DIRS_RELEASE}"
                              "${libconfig_libconfig_libconfig_BIN_DIRS_RELEASE}" # package_bindir
                              "${libconfig_libconfig_libconfig_LIBRARY_TYPE_RELEASE}"
                              "${libconfig_libconfig_libconfig_IS_HOST_WINDOWS_RELEASE}"
                              libconfig_libconfig_libconfig_DEPS_TARGET
                              libconfig_libconfig_libconfig_LIBRARIES_TARGETS
                              "_RELEASE"
                              "libconfig_libconfig_libconfig"
                              "${libconfig_libconfig_libconfig_NO_SONAME_MODE_RELEASE}")


        ########## TARGET PROPERTIES #####################################
        set_property(TARGET libconfig::libconfig
                     APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                     $<$<CONFIG:Release>:${libconfig_libconfig_libconfig_OBJECTS_RELEASE}>
                     $<$<CONFIG:Release>:${libconfig_libconfig_libconfig_LIBRARIES_TARGETS}>
                     )

        if("${libconfig_libconfig_libconfig_LIBS_RELEASE}" STREQUAL "")
            # If the component is not declaring any "cpp_info.components['foo'].libs" the system, frameworks etc are not
            # linked to the imported targets and we need to do it to the global target
            set_property(TARGET libconfig::libconfig
                         APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                         libconfig_libconfig_libconfig_DEPS_TARGET)
        endif()

        set_property(TARGET libconfig::libconfig APPEND PROPERTY INTERFACE_LINK_OPTIONS
                     $<$<CONFIG:Release>:${libconfig_libconfig_libconfig_LINKER_FLAGS_RELEASE}>)
        set_property(TARGET libconfig::libconfig APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES
                     $<$<CONFIG:Release>:${libconfig_libconfig_libconfig_INCLUDE_DIRS_RELEASE}>)
        set_property(TARGET libconfig::libconfig APPEND PROPERTY INTERFACE_LINK_DIRECTORIES
                     $<$<CONFIG:Release>:${libconfig_libconfig_libconfig_LIB_DIRS_RELEASE}>)
        set_property(TARGET libconfig::libconfig APPEND PROPERTY INTERFACE_COMPILE_DEFINITIONS
                     $<$<CONFIG:Release>:${libconfig_libconfig_libconfig_COMPILE_DEFINITIONS_RELEASE}>)
        set_property(TARGET libconfig::libconfig APPEND PROPERTY INTERFACE_COMPILE_OPTIONS
                     $<$<CONFIG:Release>:${libconfig_libconfig_libconfig_COMPILE_OPTIONS_RELEASE}>)


    ########## AGGREGATED GLOBAL TARGET WITH THE COMPONENTS #####################
    set_property(TARGET libconfig::libconfig APPEND PROPERTY INTERFACE_LINK_LIBRARIES libconfig::libconfig++)
    set_property(TARGET libconfig::libconfig APPEND PROPERTY INTERFACE_LINK_LIBRARIES libconfig::libconfig)

########## For the modules (FindXXX)
set(libconfig_LIBRARIES_RELEASE libconfig::libconfig)
