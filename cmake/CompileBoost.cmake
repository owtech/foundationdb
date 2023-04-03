## Boost

# iostreams can include support for different compression libraries,
# but this boost build disables them all except for basic zlib.
# To use a separate boost build for fdb, do not build with bzip2/lzma/zstd
# enabled, or add appropriate link flags via cmake options.

function(compile_boost)
  # Initialize function incoming parameters
  set(options)
  set(oneValueArgs TARGET)
  set(multiValueArgs BUILD_ARGS CXXFLAGS LDFLAGS)
  cmake_parse_arguments(COMPILE_BOOST "${options}" "${oneValueArgs}"
                          "${multiValueArgs}" ${ARGN} )

  # Configure bootstrap command
  set(BOOTSTRAP_COMMAND "./bootstrap.sh")
  set(BOOTSTRAP_LIBRARIES "context,filesystem,iostreams")

  set(BOOST_CXX_COMPILER "${CMAKE_CXX_COMPILER}")
  # Can't build Boost with Intel compiler, use clang instead.
  if(ICX)
    execute_process (
      COMMAND bash -c "which clang++ | tr -d '\n'"
      OUTPUT_VARIABLE BOOST_CXX_COMPILER
    )
    set(BOOST_TOOLSET "clang")
  elseif(CLANG)
    set(BOOST_TOOLSET "clang")
  else()
    set(BOOST_TOOLSET "gcc")
  endif()
  message(STATUS "Use ${BOOST_TOOLSET} to build boost")

  # Configure b2 command
  # b2's clang-darwin toolset passes an explicit --target, which suppresses Apple
  # clang's SDK inference, and macOS keeps no C++ headers outside the SDK. Hand b2
  # the same sysroot the main build uses so boost compiles against an identical SDK.
  set(B2_COMMAND "./b2")
  if(APPLE AND CMAKE_OSX_SYSROOT)
    set(B2_COMMAND env "SDKROOT=${CMAKE_OSX_SYSROOT}" "./b2")
  endif()
  set(BOOST_COMPILER_FLAGS -fvisibility=hidden -fPIC -std=c++17 --no-warnings)
  set(BOOST_LINK_FLAGS "")
  if(APPLE OR USE_LIBCXX)
    list(APPEND BOOST_COMPILER_FLAGS -stdlib=libc++ -nostdlib++)
    list(APPEND BOOST_LINK_FLAGS -lc++ -lc++abi)
    if (NOT APPLE)
      list(APPEND BOOST_LINK_FLAGS -static-libgcc)
    endif()
  endif()

  # Update the user-config.jam
  set(BOOST_ADDITIONAL_COMPILE_OPTIONS "")
  foreach(flag IN LISTS BOOST_COMPILER_FLAGS COMPILE_BOOST_CXXFLAGS)
    string(APPEND BOOST_ADDITIONAL_COMPILE_OPTIONS "<cxxflags>${flag} ")
  endforeach()
  foreach(flag IN LISTS BOOST_LINK_FLAGS COMPILE_BOOST_LDFLAGS)
    string(APPEND BOOST_ADDITIONAL_COMPILE_OPTIONS "<linkflags>${flag} ")
  endforeach()

  # CMake can't expand generator expressions without a target to model against.
  # Create a fake C++ target here and use `file(GENERATE ...)` to expand the
  # flags appropriately for the boost build system and emit this file instead of
  # configure_file.
  # TODO: CMake runs into a memory corruption bug while trying to expand
  #       generated files with the `file(GENERATE TARGET ...)` API. Once that is
  #       fixed, we can drop the boost-options lists to reduce maintenance
  #       burden.
  configure_file(${CMAKE_SOURCE_DIR}/cmake/user-config.jam.cmake ${CMAKE_BINARY_DIR}/user-config.jam)
  set(USER_CONFIG_FLAG --user-config=${CMAKE_BINARY_DIR}/user-config.jam)

  # Build boost
  include(ExternalProject)

  set(BOOST_SRC_URL https://archives.boost.io/release/1.89.0/source/boost_1_89_0.tar.bz2)
  set(BOOST_SRC_SHA SHA256=85a33fa22621b4f314f8e85e1a5e2a9363d22e4f4992925d4bb3bc631b5a0c7a)

  if(USE_ASAN)
    set(B2_ADDTTIONAL_BUILD_ARGS context-impl=ucontext)
  endif()
  set(BOOST_INSTALL_DIR "${CMAKE_BINARY_DIR}/boost_install")
  ExternalProject_add("${COMPILE_BOOST_TARGET}Project"
    URL "https://boostorg.jfrog.io/artifactory/main/release/1.78.0/source/boost_1_78_0.tar.bz2"
    URL_HASH SHA256=8681f175d4bdb26c52222665793eef08490d7758529330f98d3b29dd0735bccc
    CONFIGURE_COMMAND ${BOOTSTRAP_COMMAND} ${BOOTSTRAP_ARGS} --with-libraries=${BOOTSTRAP_LIBRARIES} --with-toolset=${BOOST_TOOLSET}
    BUILD_COMMAND ${B2_COMMAND} toolset=${BOOST_TOOLSET} link=static ${COMPILE_BOOST_BUILD_ARGS} --prefix=${BOOST_INSTALL_DIR} ${USER_CONFIG_FLAG} install
    BUILD_IN_SOURCE ON
    INSTALL_COMMAND ""
    UPDATE_COMMAND ""
    BUILD_BYPRODUCTS "${BOOST_INSTALL_DIR}/boost/config.hpp"
                     "${BOOST_INSTALL_DIR}/lib/libboost_context.a"
                     "${BOOST_INSTALL_DIR}/lib/libboost_filesystem.a"
                     "${BOOST_INSTALL_DIR}/lib/libboost_iostreams.a")

  add_library(${COMPILE_BOOST_TARGET}_context STATIC IMPORTED)
  add_dependencies(${COMPILE_BOOST_TARGET}_context ${COMPILE_BOOST_TARGET}Project)
  set_target_properties(${COMPILE_BOOST_TARGET}_context PROPERTIES IMPORTED_LOCATION "${BOOST_INSTALL_DIR}/lib/libboost_context.a")

  add_library(${COMPILE_BOOST_TARGET}_filesystem STATIC IMPORTED)
  add_dependencies(${COMPILE_BOOST_TARGET}_filesystem ${COMPILE_BOOST_TARGET}Project)
  set_target_properties(${COMPILE_BOOST_TARGET}_filesystem PROPERTIES IMPORTED_LOCATION "${BOOST_INSTALL_DIR}/lib/libboost_filesystem.a")

  add_library(${COMPILE_BOOST_TARGET}_iostreams STATIC IMPORTED)
  add_dependencies(${COMPILE_BOOST_TARGET}_iostreams ${COMPILE_BOOST_TARGET}Project)
  set_target_properties(${COMPILE_BOOST_TARGET}_iostreams PROPERTIES IMPORTED_LOCATION "${BOOST_INSTALL_DIR}/lib/libboost_iostreams.a")

  add_library(${COMPILE_BOOST_TARGET} INTERFACE)
  target_include_directories(${COMPILE_BOOST_TARGET} SYSTEM INTERFACE ${BOOST_INSTALL_DIR}/include)
  target_link_libraries(${COMPILE_BOOST_TARGET} INTERFACE ${COMPILE_BOOST_TARGET}_context ${COMPILE_BOOST_TARGET}_filesystem ${COMPILE_BOOST_TARGET}_iostreams)

endfunction(compile_boost)

if(USE_SANITIZER)
  if(WIN32)
    message(FATAL_ERROR "Sanitizers are not supported on Windows")
  endif()
  message(STATUS "A sanitizer is enabled, need to build boost from source")
  if (USE_VALGRIND)
    compile_boost(TARGET boost_target BUILD_ARGS valgrind=on
      CXXFLAGS ${BOOST_CXX_OPTIONS} LDFLAGS ${BOOST_LINK_OPTIONS})
  elseif(USE_ASAN)
    list(APPEND BOOST_CXX_OPTIONS -DBOOST_COROUTINES_NO_DEPRECATION_WARNING)
    compile_boost(TARGET boost_target BUILD_ARGS
      CXXFLAGS ${BOOST_CXX_OPTIONS} LDFLAGS ${BOOST_LINK_OPTIONS})
  else()
    compile_boost(TARGET boost_target BUILD_ARGS context-impl=ucontext
      CXXFLAGS ${BOOST_COMPILE_OPTIONS} LDFLAGS ${BOOST_LINK_OPTIONS})
  endif()
  return()
endif()

# since boost 1.72 boost installs cmake configs. We will enforce config mode
set(Boost_USE_STATIC_LIBS ON)

# Clang and Gcc will have different name mangling to std::call_once, etc.
if (UNIX AND CMAKE_CXX_COMPILER_ID MATCHES "Clang$")
  list(APPEND CMAKE_PREFIX_PATH /opt/boost_1_78_0_clang)
  set(BOOST_HINT_PATHS /opt/boost_1_78_0_clang)
  message(STATUS "Using Clang version of boost::context boost::filesystem and boost::iostreams")
else ()
  list(APPEND CMAKE_PREFIX_PATH /opt/boost_1_78_0)
  set(BOOST_HINT_PATHS /opt/boost_1_78_0)
  message(STATUS "Using g++ version of boost::context boost::filesystem and boost::iostreams")
endif ()

if(BOOST_ROOT)
  list(APPEND BOOST_HINT_PATHS ${BOOST_ROOT})
endif()

if(WIN32)
  # this should be done with the line below -- but apparently the CI is not set up
  # properly for config mode. So we use the old way on Windows
  #  find_package(Boost 1.72.0 EXACT QUIET REQUIRED CONFIG PATHS ${BOOST_HINT_PATHS})
  # I think depending on the cmake version this will cause weird warnings
  find_package(Boost 1.72 COMPONENTS filesystem iostreams)
  add_library(boost_target INTERFACE)
  target_link_libraries(boost_target INTERFACE Boost::boost Boost::filesystem Boost::iostreams)
  return()
endif()

find_package(Boost 1.78.0 EXACT QUIET COMPONENTS context filesystem iostreams CONFIG PATHS ${BOOST_HINT_PATHS})
set(FORCE_BOOST_BUILD OFF CACHE BOOL "Forces cmake to build boost and ignores any installed boost")

if(Boost_FOUND AND Boost_filesystem_FOUND AND Boost_context_FOUND AND Boost_iostreams_FOUND AND NOT FORCE_BOOST_BUILD)
  add_library(boost_target INTERFACE)
  target_link_libraries(boost_target INTERFACE Boost::boost Boost::context Boost::filesystem Boost::iostreams)
elseif(WIN32)
  message(FATAL_ERROR "Could not find Boost")
else()
  if(FORCE_BOOST_BUILD)
    message(STATUS "Compile boost because FORCE_BOOST_BUILD is set")
  else()
    message(STATUS "Didn't find Boost -- will compile from source")
  endif()
  compile_boost(TARGET boost_target)
endif()
