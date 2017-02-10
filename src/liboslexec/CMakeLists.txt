SET ( liboslexec_srcs
          shadingsys.cpp closure.cpp
          dictionary.cpp
          context.cpp instance.cpp
          loadshader.cpp master.cpp
          opcolor.cpp opmatrix.cpp opmessage.cpp
          opnoise.cpp
          opspline.cpp opstring.cpp optexture.cpp
          oslexec.cpp
          pointcloud.cpp rendservices.cpp
          constfold.cpp runtimeoptimize.cpp typespec.cpp
          lpexp.cpp lpeparse.cpp automata.cpp accum.cpp
          opclosure.cpp
          shadeimage.cpp
          backendllvm.cpp
          llvm_gen.cpp llvm_instance.cpp llvm_util.cpp
    )

# oslcomp symbols used in oslexec
if (NOT BUILDSTATIC)
    LIST(APPEND liboslexec_srcs
        ../liboslcomp/ast.cpp
        ../liboslcomp/codegen.cpp
        ../liboslcomp/oslcomp.cpp
        ../liboslcomp/symtab.cpp
        ../liboslcomp/typecheck.cpp
        )
endif ()

# oslnoise symbols used in oslexec
LIST(APPEND liboslexec_srcs
   ../liboslnoise/gabornoise.cpp
   ../liboslnoise/simplexnoise.cpp
   )

# oslquery symbols used in oslexec
if (NOT BUILDSTATIC)
    LIST(APPEND liboslexec_srcs
        ../liboslquery/oslquery.cpp
        )
endif ()

include_directories ( "${CMAKE_SOURCE_DIR}/src/liboslcomp" )

FILE ( GLOB exec_headers "*.h" )
FILE ( GLOB compiler_headers "../liboslcomp/*.h" )

FLEX_BISON ( osolex.l osogram.y oso liboslexec_srcs exec_headers )
FLEX_BISON ( ../liboslcomp/osllex.l ../liboslcomp/oslgram.y osl liboslexec_srcs compiler_headers )

SET ( CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D__STDC_LIMIT_MACROS -D__STDC_CONSTANT_MACROS" )

MACRO ( LLVM_COMPILE llvm_src llvm_bc_cpp_file )
    GET_FILENAME_COMPONENT ( llvmsrc_we ${llvm_src} NAME_WE )
    SET ( llvm_asm "${CMAKE_CURRENT_BINARY_DIR}/${llvmsrc_we}.ll" )
    SET ( llvm_bc "${CMAKE_CURRENT_BINARY_DIR}/${llvmsrc_we}_bc" )
    SET ( llvm_bc_h "${llvm_bc}.h" )
    SET ( llvm_bc_cpp "${llvm_bc}.cpp" )
    SET ( ${llvm_bc_cpp_file} ${llvm_bc_cpp} )
    if (VERBOSE)
        MESSAGE (STATUS "LLVM_COMPILE in=${llvm_src}")
        MESSAGE (STATUS "LLVM_COMPILE asm=${llvm_asm}")
        MESSAGE (STATUS "LLVM_COMPILE h=${llvm_bc_h}")
        MESSAGE (STATUS "LLVM_COMPILE cpp=${llvm_bc_cpp}")
    endif ()
    if (OSL_NAMESPACE)
        SET (LLVM_COMPILE_FLAGS ${LLVM_COMPILE_FLAGS} "-DOSL_NAMESPACE=${OSL_NAMESPACE}")
    endif ()
    if (OPENIMAGEIO_NAMESPACE)
        SET (LLVM_COMPILE_FLAGS ${LLVM_COMPILE_FLAGS} "-DOPENIMAGEIO_NAMESPACE=${OPENIMAGEIO_NAMESPACE}")
    endif ()
    get_property (CURRENT_DEFINITIONS DIRECTORY PROPERTY COMPILE_DEFINITIONS)
    if (VERBOSE)
        message (STATUS "Current #defines are ${CURRENT_DEFINITIONS}")
    endif ()
    foreach (def ${CURRENT_DEFINITIONS})
        set (LLVM_COMPILE_FLAGS ${LLVM_COMPILE_FLAGS} "-D${def}")
    endforeach()
    set (LLVM_COMPILE_FLAGS ${LLVM_COMPILE_FLAGS} ${OSL_SIMD_FLAGS} ${OSL_CSTD_FLAGS})

    # Figure out what program we will use to make the bitcode.
    if (NOT LLVM_BC_GENERATOR)
        FIND_PROGRAM(LLVM_BC_GENERATOR NAMES "clang++" PATHS "${LLVM_DIRECTORY}/bin" NO_DEFAULT_PATH NO_CMAKE_SYSTEM_PATH NO_SYSTEM_ENVIRONMENT_PATH NO_CMAKE_ENVIRONMENT_PATH NO_CMAKE_PATH)
        # If that didn't work, look anywhere
        if (NOT LLVM_BC_GENERATOR)
            # Wasn't in their build, look anywhere
            FIND_PROGRAM(LLVM_BC_GENERATOR NAMES clang++ llvm-g++)
        endif ()
        if (NOT LLVM_BC_GENERATOR)
            message (FATAL_ERROR "You must have a valid llvm bitcode generator (clang++) somewhere.")
        endif ()
        set (llvm_bc_generator_custom FALSE)
    else ()
        set( llvm_bc_generator_custom TRUE)
    endif ()

    if (VERBOSE)
        message (STATUS "Using ${LLVM_BC_GENERATOR} to generate bitcode.")
    endif()

    # Fix specific problem I had on new Apple systems (e.g. Mavericks) with
    # LLVM/libc++ installed -- for some reason, LLVM 3.4 wasn't finding it,
    # so in that specific case, append another -I to point it in the right
    # direction.
    if (APPLE AND NOT llvm_bc_generator_custom AND ${LLVM_BC_GENERATOR} MATCHES ".*clang.*")
        EXEC_PROGRAM ( "${LLVM_BC_GENERATOR}" ARGS --version OUTPUT_VARIABLE MY_CLANG_VERSION )
        string (REGEX REPLACE "clang version ([0-9][.][0-9]+).*" "\\1" MY_CLANG_VERSION "${MY_CLANG_VERSION}")
        if ((${MY_CLANG_VERSION} VERSION_GREATER "3.3")
              AND (EXISTS "/usr/lib/libc++.dylib")
              AND (EXISTS "/Library/Developer/CommandLineTools/usr/lib/c++/v1"))
            set (LLVM_COMPILE_FLAGS ${LLVM_COMPILE_FLAGS} "-I/Library/Developer/CommandLineTools/usr/lib/c++/v1")
        endif ()
    endif ()

    # Command to turn the .cpp file into an LLVM IR file (.ll)
    ADD_CUSTOM_COMMAND ( OUTPUT ${llvm_asm}
      PRE_BUILD
      COMMAND ${LLVM_BC_GENERATOR}
          ${LLVM_COMPILE_FLAGS}
          "-I${CMAKE_CURRENT_SOURCE_DIR}"
          "-I${CMAKE_SOURCE_DIR}/src/include"
          "-I${CMAKE_BINARY_DIR}/include/OSL"
          "-I${OPENIMAGEIO_INCLUDE_DIR}"
          "-I${ILMBASE_INCLUDE_DIR}"
          "-I${Boost_INCLUDE_DIRS}"
          -DOSL_COMPILING_TO_BITCODE=1
          -Wno-deprecated-register
          -O3 -fno-math-errno -S -emit-llvm -o ${llvm_asm} ${llvm_src}
      MAIN_DEPENDENCY ${llvm_src}
      DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/serialize-bc.bash"
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" )

    if (OSL_SPLIT_BITCODES)
      ADD_DEFINITIONS (-DOSL_SPLIT_BITCODES=1)
      SET ( llvm_to_cxx_msg "Splitting bytecode into separate buffers" )
      SET ( llvm_to_cxx_arg -func=osl_ -func=OSL:: -func=OpenImageIO:: )
    else ()
      SET ( llvm_to_cxx_msg "Generating monolithic bytecode" )
      SET ( llvm_to_cxx_arg "" )
    endif ()

    # Command to turn the .ll file into a C++ file of bitcode buffers, possibly
    # splitting each function into its own buffer (-DOSL_SPLIT_BITCODES=1).
    ADD_EXECUTABLE ( llvm_to_cxx llvm_to_cxx.cpp)
    TARGET_LINK_LIBRARIES ( llvm_to_cxx ${LLVM_LIBRARY} ${LLVM_LIBRARIES}
                            ${LLVM_LDFLAGS} ${LLVM_SYSTEM_LIBRARIES} ${LLVM_READER_LIBRARIES} )
    TARGET_INCLUDE_DIRECTORIES ( llvm_to_cxx PRIVATE ${CMAKE_BINARY_DIR}/include/OSL )
    TARGET_COMPILE_OPTIONS ( llvm_to_cxx PRIVATE ${LLVM_COMPILE_FLAGS} )
    ADD_CUSTOM_COMMAND ( OUTPUT "${llvm_bc_h}" "${llvm_bc_cpp}"
                         COMMAND llvm_to_cxx
                         ARGS ${llvm_asm} ${llvm_to_cxx_arg} -prefix=osl_llvm_compiled_ops -o ${llvm_bc}
                         MAIN_DEPENDENCY ${llvm_src}
                         DEPENDS ${llvm_asm} llvm_to_cxx
                         COMMENT "${llvm_to_cxx_msg}"
                         WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
                         VERBATIM )
ENDMACRO ( )


if (USE_LLVM_BITCODE)
    LLVM_COMPILE ( llvm_ops.cpp llvm_bc_cpp_file )
    LIST(APPEND liboslexec_srcs ${llvm_bc_cpp_file})
else ()
    # With MSVC/Mingw, we don't compile llvm_ops.cpp to LLVM bitcode, due
    # to clang being unable to compile MSVC C++ header files at this time.
    # Instead it is part of the regular build process.
    ADD_DEFINITIONS (-DOSL_LLVM_NO_BITCODE)
    SET (liboslexec_srcs ${liboslexec_srcs} llvm_ops.cpp)
endif ()

if (BUILDSTATIC)
    ADD_LIBRARY ( oslexec STATIC ${liboslexec_srcs} )
else ()
    ADD_LIBRARY ( oslexec SHARED ${liboslexec_srcs} )
endif ()

TARGET_LINK_LIBRARIES ( oslexec
                        ${VISIBILITY_COMMAND} ${VISIBILITY_MAP_COMMAND}
                        ${OPENIMAGEIO_LIBRARIES} ${ILMBASE_LIBRARIES}
                        ${PUGIXML_LIBRARIES}
                        ${PARTIO_LIBRARIES} ${ZLIB_LIBRARIES}
                        ${Boost_LIBRARIES} ${CMAKE_DL_LIBS}
                        ${LLVM_LIBRARY} ${LLVM_MCJIT_LIBRARY}
                        ${LLVM_LIBRARIES} ${LLVM_LDFLAGS}
                        ${LLVM_SYSTEM_LIBRARIES})
ADD_DEPENDENCIES (oslexec "${CMAKE_CURRENT_SOURCE_DIR}/liboslexec.map")

INSTALL ( TARGETS oslexec RUNTIME DESTINATION bin LIBRARY DESTINATION lib ARCHIVE DESTINATION lib )

# Unit tests
if (OSL_BUILD_TESTS)
    add_executable (accum_test accum_test.cpp)
    target_link_libraries ( accum_test oslexec oslcomp ${Boost_LIBRARIES} ${CMAKE_DL_LIBS})
    add_test (unit_accum "${CMAKE_BINARY_DIR}/src/liboslexec/accum_test")

    add_executable (llvmutil_test llvmutil_test.cpp)
    target_link_libraries ( llvmutil_test oslexec oslcomp ${Boost_LIBRARIES} ${CMAKE_DL_LIBS})
    add_test (unit_llvmutil "${CMAKE_BINARY_DIR}/src/liboslexec/llvmutil_test")
endif ()
