###########################################################################
# Find libraries

# When not in VERBOSE mode, try to make things as quiet as possible
if (NOT VERBOSE)
    set (Bison_FIND_QUIETLY true CACHE BOOL "Find bison verbosely.")
    set (Boost_FIND_QUIETLY true CACHE BOOL "Find boost verbosely.")
    set (Curses_FIND_QUIETLY true CACHE BOOL "Find curses verbosely.")
    set (Flex_FIND_QUIETLY true CACHE BOOL "Find flex verbosely.")
    set (LLVM_FIND_QUIETLY true CACHE BOOL "Find LLVM verbosely.")
    set (OpenEXR_FIND_QUIETLY true CACHE BOOL "Find OpenExr verbosely.")
    set (OpenImageIO_FIND_QUIETLY true CACHE BOOL "Find OpenImageIO verbosely.")
    set (Partio_FIND_QUIETLY true CACHE BOOL "Find Partio verbosely.")
    set (PkgConfig_FIND_QUIETLY true CACHE BOOL "Find PkgConfig verbosely.")
    set (PugiXML_FIND_QUIETLY TRUE CACHE BOOL "Find PlugiXML verbosely.")
    set (PythonInterp_FIND_QUIETLY true CACHE BOOL "Find Python binary verbosely.")
    set (PythonLibs_FIND_QUIETLY true CACHE BOOL "Find Python libraries verbosely.")
    set (Threads_FIND_QUIETLY true CACHE BOOL "Find Threads verbosely.")
    set (ZLIB_FIND_QUIETLY true CACHE BOOL "Find zlib verbosely.")
endif ()


setup_string (SPECIAL_COMPILE_FLAGS ""
               "Custom compilation flags")
if (SPECIAL_COMPILE_FLAGS)
    add_definitions (${SPECIAL_COMPILE_FLAGS})
endif ()



###########################################################################
# IlmBase setup

find_package (OpenEXR REQUIRED)
#OpenEXR 2.2 still has problems with importing ImathInt64.h unqualified
#thus need for ilmbase/OpenEXR
include_directories ("${OPENEXR_INCLUDE_DIR}"
                     "${ILMBASE_INCLUDE_DIR}"
                     "${ILMBASE_INCLUDE_DIR}/OpenEXR")
if (${OPENEXR_VERSION} VERSION_LESS 2.0.0)
    # OpenEXR 1.x had weird #include dirctives, this is also necessary:
    include_directories ("${OPENEXR_INCLUDE_DIR}/OpenEXR")
else ()
    add_definitions (-DUSE_OPENEXR_VERSION2=1)
endif ()
if (NOT OpenEXR_FIND_QUIETLY)
    message (STATUS "ILMBASE_INCLUDE_DIR = ${ILMBASE_INCLUDE_DIR}")
    message (STATUS "ILMBASE_LIBRARIES = ${ILMBASE_LIBRARIES}")
endif ()

# end IlmBase setup
###########################################################################


###########################################################################
# Boost setup

if (NOT Boost_FIND_QUIETLY)
    message (STATUS "BOOST_ROOT ${BOOST_ROOT}")
endif ()

if (NOT DEFINED Boost_ADDITIONAL_VERSIONS)
  set (Boost_ADDITIONAL_VERSIONS "1.63" "1.62" "1.61" "1.60"
                                 "1.59" "1.58" "1.57" "1.56" "1.55")
endif ()
if (LINKSTATIC)
    set (Boost_USE_STATIC_LIBS   ON)
endif ()
set (Boost_USE_MULTITHREADED ON)
if (BOOST_CUSTOM)
    set (Boost_FOUND true)
    # N.B. For a custom version, the caller had better set up the variables
    # Boost_VERSION, Boost_INCLUDE_DIRS, Boost_LIBRARY_DIRS, Boost_LIBRARIES.
else ()
    set (Boost_COMPONENTS regex system thread wave)
    find_package (Boost 1.55 REQUIRED
                  COMPONENTS ${Boost_COMPONENTS}
                 )
endif ()

# On Linux, Boost 1.55 and higher seems to need to link against -lrt
if (CMAKE_SYSTEM_NAME MATCHES "Linux" AND ${Boost_VERSION} GREATER 105499)
    list (APPEND Boost_LIBRARIES "rt")
endif ()

if (NOT Boost_FIND_QUIETLY)
    message (STATUS "BOOST_ROOT ${BOOST_ROOT}")
    message (STATUS "Boost found ${Boost_FOUND} ")
    message (STATUS "Boost version      ${Boost_VERSION}")
    message (STATUS "Boost include dirs ${Boost_INCLUDE_DIRS}")
    message (STATUS "Boost library dirs ${Boost_LIBRARY_DIRS}")
    message (STATUS "Boost libraries    ${Boost_LIBRARIES}")
endif ()

include_directories (SYSTEM "${Boost_INCLUDE_DIRS}")
link_directories ("${Boost_LIBRARY_DIRS}")

# end Boost setup
###########################################################################


###########################################################################
# OpenImageIO

find_package (OpenImageIO 1.7 REQUIRED)
include_directories ("${OPENIMAGEIO_INCLUDE_DIR}")
message (STATUS "Using OpenImageIO ${OPENIMAGEIO_VERSION}")

# end OpenImageIO setup
###########################################################################


###########################################################################
# Partio

find_package (ZLIB)
if (USE_PARTIO)
    find_package (Partio)
    if (PARTIO_FOUND)
        add_definitions ("-DUSE_PARTIO=1")
        include_directories ("${PARTIO_INCLUDE_DIR}")
    else ()
        add_definitions ("-DUSE_PARTIO=0")
    endif ()
endif (USE_PARTIO)

# end Partio setup
###########################################################################


###########################################################################
# Pugixml setup.  Normally we just use the version bundled with oiio, but
# some linux distros are quite particular about having separate packages so we
# allow this to be overridden to use the distro-provided package if desired.
if (USE_EXTERNAL_PUGIXML)
    find_package (PugiXML REQUIRED)
    # insert include path to pugixml first, to ensure that the external
    # pugixml is found, and not the one in OIIO's include directory.
    include_directories (BEFORE "${PUGIXML_INCLUDE_DIR}")
endif()
# end Pugixml setup
###########################################################################


###########################################################################
# LLVM library setup

# try to find llvm-config, with a specific version if specified
if (LLVM_DIRECTORY)
    # Force path expansion for LLVM_DIRECTORY (i.e. ~/path/to/llvm)
    get_filename_component(LLVM_DIRECTORY ${LLVM_DIRECTORY} REALPATH)

    set (LLVM_CONFIG_PATH_HINTS "${LLVM_DIRECTORY}/bin")
endif ()
list (APPEND LLVM_CONFIG_PATH_HINTS
        "/usr/local/opt/llvm/${LLVM_VERSION}/bin/"
        "/usr/local/opt/llvm/bin/")
find_program (LLVM_CONFIG
              NAMES llvm-config-${LLVM_VERSION} llvm-config
              HINTS ${LLVM_CONFIG_PATH_HINTS} NO_DEFAULT_PATH)
find_program (LLVM_CONFIG
              NAMES llvm-config-${LLVM_VERSION} llvm-config
              HINTS ${LLVM_CONFIG_PATH_HINTS})
if (VERBOSE)
    message (STATUS "Found llvm-config '${LLVM_CONFIG}'")
endif ()

if (NOT LLVM_DIRECTORY)
    execute_process (COMMAND ${LLVM_CONFIG} --prefix
           OUTPUT_VARIABLE LLVM_DIRECTORY
           OUTPUT_STRIP_TRAILING_WHITESPACE)
endif()

execute_process (COMMAND ${LLVM_CONFIG} --version
       OUTPUT_VARIABLE LLVM_VERSION
       OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process (COMMAND ${LLVM_CONFIG} --libdir
       OUTPUT_VARIABLE LLVM_LIB_DIR
       OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process (COMMAND ${LLVM_CONFIG} --includedir
       OUTPUT_VARIABLE LLVM_INCLUDES
       OUTPUT_STRIP_TRAILING_WHITESPACE)
if (NOT ${LLVM_VERSION} VERSION_LESS 3.8)
    execute_process (COMMAND ${LLVM_CONFIG} --system-libs
                     OUTPUT_VARIABLE LLVM_SYSTEM_LIBRARIES
                     OUTPUT_STRIP_TRAILING_WHITESPACE)
else ()
    # Older LLVM did not have llvm-config --system-libs, but we know that
    # on Linux, we'll need curses.
    find_package (Curses)
    if (CURSES_FOUND)
        list (APPEND LLVM_SYSTEM_LIBRARIES ${CURSES_LIBRARIES})
    endif ()
endif ()

find_library ( LLVM_LIBRARY
               NAMES LLVM-${LLVM_VERSION} LLVM
               PATHS ${LLVM_LIB_DIR})
find_library ( LLVM_MCJIT_LIBRARY
               NAMES LLVMMCJIT
               PATHS ${LLVM_LIB_DIR})

if (NOT LLVM_LIBRARY)
    # Don't really know the cutoff when passes works as an argument
    if (LLVM_VERSION VERSION_LESS 3.5.0)
        execute_process (COMMAND ${LLVM_CONFIG} --libfiles engine ipo bitwriter bitreader linker
                     OUTPUT_VARIABLE LLVM_LIBRARIES
                     OUTPUT_STRIP_TRAILING_WHITESPACE)
        execute_process (COMMAND ${LLVM_CONFIG} --libfiles irreader
                     OUTPUT_VARIABLE LLVM_READER_LIBRARIES
                     OUTPUT_STRIP_TRAILING_WHITESPACE)
        string (REPLACE " " ";" LLVM_READER_LIBRARIES "${LLVM_READER_LIBRARIES}")
    else ()
        execute_process (COMMAND ${LLVM_CONFIG} --libfiles engine passes
                         OUTPUT_VARIABLE LLVM_LIBRARIES
                         OUTPUT_STRIP_TRAILING_WHITESPACE)
        set (LLVM_READER_LIBRARIES "" )
    endif ()
endif ()

# shared llvm library may not be available, this is not an error if we use LLVM_STATIC.
if ((LLVM_LIBRARY OR LLVM_LIBRARIES OR LLVM_STATIC) AND LLVM_INCLUDES AND LLVM_DIRECTORY AND LLVM_LIB_DIR)
  # ensure include directory is added (in case of non-standard locations
  include_directories (BEFORE "${LLVM_INCLUDES}")

  # See if building against an LLVM tree then, and if so add the path to the
  # generated headers
  execute_process (COMMAND ${LLVM_CONFIG} --src-root
                     OUTPUT_VARIABLE llvm_src_root
                     OUTPUT_STRIP_TRAILING_WHITESPACE)
  if (LLVM_INCLUDES STREQUAL ${llvm_src_root}/include)
    if (NOT LLVM_FIND_QUIETLY)
      message (STATUS "Detected LLVM build tree, adding additional include paths")
    endif ()
    if (NOT LLVM_BC_GENERATOR)
        FIND_PROGRAM(LLVM_BC_GENERATOR NAMES "clang++" PATHS "${LLVM_DIRECTORY}/bin" NO_DEFAULT_PATH NO_CMAKE_SYSTEM_PATH NO_SYSTEM_ENVIRONMENT_PATH NO_CMAKE_ENVIRONMENT_PATH NO_CMAKE_PATH)
        if (LLVM_BC_GENERATOR AND NOT LLVM_FIND_QUIETLY)
          message (STATUS "Using LLVM bitcode generator: ${LLVM_BC_GENERATOR}")
        endif ()
    endif ()
    include_directories (BEFORE "${LLVM_DIRECTORY}/include")
  endif ()

  if (NOT OSL_LLVM_VERSION)
      # Extract and concatenate major & minor, remove wayward patches,
      # dots, and "svn" or other suffixes.
      string (REGEX REPLACE "([0-9]+)\\.([0-9]+).*" "\\1\\2" OSL_LLVM_VERSION ${LLVM_VERSION})
  endif ()
  add_definitions (-DOSL_LLVM_VERSION=${OSL_LLVM_VERSION})
  add_definitions (-DOSL_LLVM_FULL_VERSION="${LLVM_VERSION}")
  link_directories ("${LLVM_LIB_DIR}")
  if (LLVM_STATIC)
    # if static LLVM libraries were requested, use llvm-config to generate
    # the list of what libraries we need, and substitute that in the right
    # way for LLVM_LIBRARY.
    execute_process (COMMAND ${LLVM_CONFIG} --libfiles
                     OUTPUT_VARIABLE LLVM_LIBRARIES
                     OUTPUT_STRIP_TRAILING_WHITESPACE)
  elseif (NOT LLVM_LIBRARIES)
    set (LLVM_LIBRARIES "${LLVM_LIBRARY}")
  endif ()

  if (LLVM_LIBRARIES)
    string (REPLACE " " ";" LLVM_LIBRARIES "${LLVM_LIBRARIES}")
    # LLVM_LIBRARIES Should be enough
    if (NOT LLVM_LIBRARY)
        set (LLVM_LIBRARY "")
    endif ()
  endif ()
endif ()

message (STATUS "LLVM version  = ${LLVM_VERSION}")
if (NOT LLVM_FIND_QUIETLY)
    message (STATUS "LLVM OSL_LLVM_VERSION = ${OSL_LLVM_VERSION}")
    message (STATUS "LLVM dir       = ${LLVM_DIRECTORY}")
    message (STATUS "LLVM includes  = ${LLVM_INCLUDES}")
    message (STATUS "LLVM lib dir   = ${LLVM_LIB_DIR}")
    message (STATUS "LLVM libraries = ${LLVM_LIBRARIES}")
    message (STATUS "LLVM sys libs  = ${LLVM_SYSTEM_LIBRARIES}")
endif ()

if (NOT LLVM_LIBRARIES AND NOT LLVM_LIBRARY)
    message (FATAL_ERROR "LLVM not found.")
endif()

# end LLVM library setup
###########################################################################
