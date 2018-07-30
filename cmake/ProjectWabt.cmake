if(ProjectWabtIncluded)
    return()
endif()
set(ProjectWabtIncluded TRUE)

include(ExternalProject)
include(GNUInstallDirs)

if(MSVC)
    # Overwrite build and install commands to force Release build on MSVC.
    set(build_command BUILD_COMMAND cmake --build <BINARY_DIR> --config Release)
    set(install_command INSTALL_COMMAND cmake --build <BINARY_DIR> --config Release --target install)
elseif(CMAKE_GENERATOR STREQUAL Ninja)
    # For Ninja we have to pass the number of jobs from CI environment.
    # Otherwise it will go crazy and run out of memory.
    if($ENV{BUILD_PARALLEL_JOBS})
        set(build_command BUILD_COMMAND cmake --build <BINARY_DIR> -- -j $ENV{BUILD_PARALLEL_JOBS})
        message(STATUS "Ninja $ENV{BUILD_PARALLEL_JOBS}")
    endif()
endif()

set(prefix ${CMAKE_BINARY_DIR}/deps)
set(source_dir ${prefix}/src)
set(binary_dir ${prefix}/src)
# Use source dir because binaryen only installs single header with C API.
set(binaryen_include_dir ${source_dir}/src)
set(binaryen_library ${prefix}/${CMAKE_INSTALL_LIBDIR}/${CMAKE_STATIC_LIBRARY_PREFIX}binaryen${CMAKE_STATIC_LIBRARY_SUFFIX})
# Include also other static libs needed:
set(binaryen_other_libraries
    ${binary_dir}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}wasm${CMAKE_STATIC_LIBRARY_SUFFIX}
    ${binary_dir}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}asmjs${CMAKE_STATIC_LIBRARY_SUFFIX}
    ${binary_dir}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}passes${CMAKE_STATIC_LIBRARY_SUFFIX}
    ${binary_dir}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}cfg${CMAKE_STATIC_LIBRARY_SUFFIX}
    ${binary_dir}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}ir${CMAKE_STATIC_LIBRARY_SUFFIX}
    ${binary_dir}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}emscripten-optimizer${CMAKE_STATIC_LIBRARY_SUFFIX}
    ${binary_dir}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}support${CMAKE_STATIC_LIBRARY_SUFFIX}
)

ExternalProject_Add(wabt
    PREFIX ${prefix}
    DOWNLOAD_NAME wabt-1.0.5.tar.gz
    DOWNLOAD_DIR ${prefix}/downloads
    SOURCE_DIR ${source_dir}
    BINARY_DIR ${binary_dir}
    URL https://github.com/WebAssembly/wabt/archive/1.0.5.tar.gz
    URL_HASH SHA256=285700512a6af1524c16422d61ae4959d4b387f2a82698198eb524b514825a8a
    CMAKE_ARGS
    -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
    -DCMAKE_BUILD_TYPE=Release
    -DBUILD_TESTS=OFF
    ${build_command}
    ${install_command}
    BUILD_BYPRODUCTS ${binaryen_library} ${binaryen_other_libraries}
)

add_library(wabt::wabt STATIC IMPORTED)

file(MAKE_DIRECTORY ${binaryen_include_dir})  # Must exist.
set_target_properties(
    wabt::wabt
    PROPERTIES
    IMPORTED_CONFIGURATIONS Release
    IMPORTED_LOCATION_RELEASE ${binaryen_library}
    INTERFACE_INCLUDE_DIRECTORIES ${binaryen_include_dir}
    INTERFACE_LINK_LIBRARIES "${binaryen_other_libraries}"

)

add_dependencies(wabt::wabt wabt)
