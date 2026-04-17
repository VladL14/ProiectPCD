########### AGGREGATED COMPONENTS AND DEPENDENCIES FOR THE MULTI CONFIG #####################
#############################################################################################

list(APPEND libconfig_COMPONENT_NAMES libconfig::libconfig libconfig::libconfig++)
list(REMOVE_DUPLICATES libconfig_COMPONENT_NAMES)
if(DEFINED libconfig_FIND_DEPENDENCY_NAMES)
  list(APPEND libconfig_FIND_DEPENDENCY_NAMES )
  list(REMOVE_DUPLICATES libconfig_FIND_DEPENDENCY_NAMES)
else()
  set(libconfig_FIND_DEPENDENCY_NAMES )
endif()

########### VARIABLES #######################################################################
#############################################################################################
set(libconfig_PACKAGE_FOLDER_RELEASE "/home/vlad/.conan2/p/b/libco5226b06e73baf/p")
set(libconfig_BUILD_MODULES_PATHS_RELEASE )


set(libconfig_INCLUDE_DIRS_RELEASE "${libconfig_PACKAGE_FOLDER_RELEASE}/include")
set(libconfig_RES_DIRS_RELEASE )
set(libconfig_DEFINITIONS_RELEASE "-DLIBCONFIGXX_STATIC"
			"-DLIBCONFIG_STATIC")
set(libconfig_SHARED_LINK_FLAGS_RELEASE )
set(libconfig_EXE_LINK_FLAGS_RELEASE )
set(libconfig_OBJECTS_RELEASE )
set(libconfig_COMPILE_DEFINITIONS_RELEASE "LIBCONFIGXX_STATIC"
			"LIBCONFIG_STATIC")
set(libconfig_COMPILE_OPTIONS_C_RELEASE )
set(libconfig_COMPILE_OPTIONS_CXX_RELEASE )
set(libconfig_LIB_DIRS_RELEASE "${libconfig_PACKAGE_FOLDER_RELEASE}/lib")
set(libconfig_BIN_DIRS_RELEASE )
set(libconfig_LIBRARY_TYPE_RELEASE STATIC)
set(libconfig_IS_HOST_WINDOWS_RELEASE 0)
set(libconfig_LIBS_RELEASE config++ config)
set(libconfig_SYSTEM_LIBS_RELEASE )
set(libconfig_FRAMEWORK_DIRS_RELEASE )
set(libconfig_FRAMEWORKS_RELEASE )
set(libconfig_BUILD_DIRS_RELEASE )
set(libconfig_NO_SONAME_MODE_RELEASE FALSE)


# COMPOUND VARIABLES
set(libconfig_COMPILE_OPTIONS_RELEASE
    "$<$<COMPILE_LANGUAGE:CXX>:${libconfig_COMPILE_OPTIONS_CXX_RELEASE}>"
    "$<$<COMPILE_LANGUAGE:C>:${libconfig_COMPILE_OPTIONS_C_RELEASE}>")
set(libconfig_LINKER_FLAGS_RELEASE
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,SHARED_LIBRARY>:${libconfig_SHARED_LINK_FLAGS_RELEASE}>"
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,MODULE_LIBRARY>:${libconfig_SHARED_LINK_FLAGS_RELEASE}>"
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:${libconfig_EXE_LINK_FLAGS_RELEASE}>")


set(libconfig_COMPONENTS_RELEASE libconfig::libconfig libconfig::libconfig++)
########### COMPONENT libconfig::libconfig++ VARIABLES ############################################

set(libconfig_libconfig_libconfig++_INCLUDE_DIRS_RELEASE "${libconfig_PACKAGE_FOLDER_RELEASE}/include")
set(libconfig_libconfig_libconfig++_LIB_DIRS_RELEASE "${libconfig_PACKAGE_FOLDER_RELEASE}/lib")
set(libconfig_libconfig_libconfig++_BIN_DIRS_RELEASE )
set(libconfig_libconfig_libconfig++_LIBRARY_TYPE_RELEASE STATIC)
set(libconfig_libconfig_libconfig++_IS_HOST_WINDOWS_RELEASE 0)
set(libconfig_libconfig_libconfig++_RES_DIRS_RELEASE )
set(libconfig_libconfig_libconfig++_DEFINITIONS_RELEASE "-DLIBCONFIGXX_STATIC")
set(libconfig_libconfig_libconfig++_OBJECTS_RELEASE )
set(libconfig_libconfig_libconfig++_COMPILE_DEFINITIONS_RELEASE "LIBCONFIGXX_STATIC")
set(libconfig_libconfig_libconfig++_COMPILE_OPTIONS_C_RELEASE "")
set(libconfig_libconfig_libconfig++_COMPILE_OPTIONS_CXX_RELEASE "")
set(libconfig_libconfig_libconfig++_LIBS_RELEASE config++)
set(libconfig_libconfig_libconfig++_SYSTEM_LIBS_RELEASE )
set(libconfig_libconfig_libconfig++_FRAMEWORK_DIRS_RELEASE )
set(libconfig_libconfig_libconfig++_FRAMEWORKS_RELEASE )
set(libconfig_libconfig_libconfig++_DEPENDENCIES_RELEASE libconfig::libconfig)
set(libconfig_libconfig_libconfig++_SHARED_LINK_FLAGS_RELEASE )
set(libconfig_libconfig_libconfig++_EXE_LINK_FLAGS_RELEASE )
set(libconfig_libconfig_libconfig++_NO_SONAME_MODE_RELEASE FALSE)

# COMPOUND VARIABLES
set(libconfig_libconfig_libconfig++_LINKER_FLAGS_RELEASE
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,SHARED_LIBRARY>:${libconfig_libconfig_libconfig++_SHARED_LINK_FLAGS_RELEASE}>
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,MODULE_LIBRARY>:${libconfig_libconfig_libconfig++_SHARED_LINK_FLAGS_RELEASE}>
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:${libconfig_libconfig_libconfig++_EXE_LINK_FLAGS_RELEASE}>
)
set(libconfig_libconfig_libconfig++_COMPILE_OPTIONS_RELEASE
    "$<$<COMPILE_LANGUAGE:CXX>:${libconfig_libconfig_libconfig++_COMPILE_OPTIONS_CXX_RELEASE}>"
    "$<$<COMPILE_LANGUAGE:C>:${libconfig_libconfig_libconfig++_COMPILE_OPTIONS_C_RELEASE}>")
########### COMPONENT libconfig::libconfig VARIABLES ############################################

set(libconfig_libconfig_libconfig_INCLUDE_DIRS_RELEASE "${libconfig_PACKAGE_FOLDER_RELEASE}/include")
set(libconfig_libconfig_libconfig_LIB_DIRS_RELEASE "${libconfig_PACKAGE_FOLDER_RELEASE}/lib")
set(libconfig_libconfig_libconfig_BIN_DIRS_RELEASE )
set(libconfig_libconfig_libconfig_LIBRARY_TYPE_RELEASE STATIC)
set(libconfig_libconfig_libconfig_IS_HOST_WINDOWS_RELEASE 0)
set(libconfig_libconfig_libconfig_RES_DIRS_RELEASE )
set(libconfig_libconfig_libconfig_DEFINITIONS_RELEASE "-DLIBCONFIG_STATIC")
set(libconfig_libconfig_libconfig_OBJECTS_RELEASE )
set(libconfig_libconfig_libconfig_COMPILE_DEFINITIONS_RELEASE "LIBCONFIG_STATIC")
set(libconfig_libconfig_libconfig_COMPILE_OPTIONS_C_RELEASE "")
set(libconfig_libconfig_libconfig_COMPILE_OPTIONS_CXX_RELEASE "")
set(libconfig_libconfig_libconfig_LIBS_RELEASE config)
set(libconfig_libconfig_libconfig_SYSTEM_LIBS_RELEASE )
set(libconfig_libconfig_libconfig_FRAMEWORK_DIRS_RELEASE )
set(libconfig_libconfig_libconfig_FRAMEWORKS_RELEASE )
set(libconfig_libconfig_libconfig_DEPENDENCIES_RELEASE )
set(libconfig_libconfig_libconfig_SHARED_LINK_FLAGS_RELEASE )
set(libconfig_libconfig_libconfig_EXE_LINK_FLAGS_RELEASE )
set(libconfig_libconfig_libconfig_NO_SONAME_MODE_RELEASE FALSE)

# COMPOUND VARIABLES
set(libconfig_libconfig_libconfig_LINKER_FLAGS_RELEASE
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,SHARED_LIBRARY>:${libconfig_libconfig_libconfig_SHARED_LINK_FLAGS_RELEASE}>
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,MODULE_LIBRARY>:${libconfig_libconfig_libconfig_SHARED_LINK_FLAGS_RELEASE}>
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:${libconfig_libconfig_libconfig_EXE_LINK_FLAGS_RELEASE}>
)
set(libconfig_libconfig_libconfig_COMPILE_OPTIONS_RELEASE
    "$<$<COMPILE_LANGUAGE:CXX>:${libconfig_libconfig_libconfig_COMPILE_OPTIONS_CXX_RELEASE}>"
    "$<$<COMPILE_LANGUAGE:C>:${libconfig_libconfig_libconfig_COMPILE_OPTIONS_C_RELEASE}>")