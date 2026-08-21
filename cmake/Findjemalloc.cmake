# Distributed under the OSI-approved Apache 2.0. See the LICENSE file in
# FoundationDB source code

#[=======================================================================[.rst:
Findjemalloc
-------

Find jemalloc, the generic purpose malloc implementation that emphasizes fragmentation avoidance and scalable concurrency support.

This module will first try jemalloc_config, then find_path and find_library.

jemalloc_ROOT variable can be used for HINTS for different version of jemalloc.

Imported targets
^^^^^^^^^^^^^^^^

This module defines the following :prop_tgt:`IMPORTED` target:

``jemalloc::jemalloc``
  The jemalloc library, if found.
``jemalloc::jemalloc_pic``
  The jemalloc_pic library, if found.

Result variables
^^^^^^^^^^^^^^^^

This module will set the following variables in your project:

``jemalloc_INCLUDE_DIRS``
  where to find jemalloc.h, etc.
``jemalloc_LIBRARY``
  the libraries to link against to use jemalloc.
``jemalloc_pic_LIBRARY``
  the libraries to link against to use jemalloc_pic.
``jemalloc_FOUND``
  If false, do not try to use jemalloc.
``jemalloc_VERSION``
  the version of the jemalloc library found
#]=======================================================================]

include(FindPackageHandleStandardArgs)
include(FindPackageMessage)

macro(_get_version)
  if(NOT DEFINED jemalloc_FIND_VERSION)
    message(FATAL_ERROR "jemalloc_FIND_VERSION is required")
    return()
  endif()

  # 1. Try dpkg first (Debian/Ubuntu systems)
  if(EXISTS "/usr/bin/dpkg")
    execute_process(
        COMMAND bash -c "dpkg -l | grep jemalloc | awk '{print \$3}' | sort -V -r | head -n 1"
        OUTPUT_VARIABLE jemalloc_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(jemalloc_VERSION)
      string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" jemalloc_VERSION "${jemalloc_VERSION}")
      if(NOT jemalloc_VERSION VERSION_LESS jemalloc_FIND_VERSION)
        message(STATUS "Found jemalloc (dpkg): ${jemalloc_VERSION}")
        return()
      endif()
    endif()
  endif() 

  # 2. Fallback to header file
  if(NOT jemalloc_VERSION AND jemalloc_INCLUDE_DIRS AND EXISTS "${jemalloc_INCLUDE_DIRS}/jemalloc.h")
    file(STRINGS "${jemalloc_INCLUDE_DIRS}/jemalloc.h" JEMALLOC_FULL_VERSION
         REGEX "^#define JEMALLOC_VERSION \"[0-9]+\\.[0-9]+\\.[0-9]+")
    if(JEMALLOC_FULL_VERSION)
      string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" jemalloc_VERSION "${JEMALLOC_FULL_VERSION}")
      if(NOT jemalloc_VERSION VERSION_LESS jemalloc_FIND_VERSION)
        message(STATUS "Found jemalloc (header): ${jemalloc_VERSION}")
        return()
      endif()
    endif()
  endif()

  # If we get here, version requirements weren't met
  if(jemalloc_VERSION)
    message(FATAL_ERROR "jemalloc version ${jemalloc_VERSION} found, but ${jemalloc_FIND_VERSION} or higher is required")
  else()
    message(FATAL_ERROR "Could not determine jemalloc version (tried dpkg and header file)")
  endif()

endmacro()

macro(_configure_use_jemalloc_config)
  # Configure per jemalloc_config
  execute_process(
    COMMAND ${_jemalloc_CONFIG_PATH} --includedir
    OUTPUT_VARIABLE jemalloc_INCLUDE_DIRS
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  execute_process(
    COMMAND ${_jemalloc_CONFIG_PATH} --libdir
    OUTPUT_VARIABLE jemalloc_LIBRARY_PATH
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  find_library(
    jemalloc_LIBRARY
    NAMES libjemalloc.a jemalloc
    HINTS ${jemalloc_LIBRARY_PATH})
  find_library(
    jemalloc_pic_LIBRARY
    NAMES libjemalloc_pic.a jemalloc_pic
    HINTS ${jemalloc_LIBRARY_PATH})
  execute_process(
    COMMAND ${_jemalloc_CONFIG_PATH} --version
    OUTPUT_VARIABLE jemalloc_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(jemalloc_INCLUDE_DIRS
     AND jemalloc_LIBRARY
     AND jemalloc_pic_LIBRARY
     AND jemalloc_VERSION)
    set(jemalloc_FOUND TRUE)
  endif()
endmacro()

macro(_configure_jemalloc_target)
  if(NOT TARGET jemalloc::jemalloc)
    add_library(jemalloc::jemalloc UNKNOWN IMPORTED)
    set_target_properties(
      jemalloc::jemalloc
      PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${jemalloc_INCLUDE_DIRS}"
                 IMPORTED_LOCATION "${jemalloc_LIBRARY}"
                 VERSION "${jemalloc_VERSION}")
  endif()
endmacro()

macro(_configure_jemalloc_pic_target)
  if(NOT TARGET jemalloc_pic::jemalloc_pic)
    add_library(jemalloc_pic::jemalloc_pic UNKNOWN IMPORTED)
    set_target_properties(
      jemalloc_pic::jemalloc_pic
      PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${jemalloc_INCLUDE_DIRS}"
                 IMPORTED_LOCATION "${jemalloc_pic_LIBRARTY}"
                 VERSION "${jemalloc_VERSION}")
  endif()
endmacro()

macro(_finalize_find_package_jemalloc)
  find_package_handle_standard_args(
    jemalloc
    FOUND_VAR jemalloc_FOUND
    REQUIRED_VARS jemalloc_INCLUDE_DIRS jemalloc_LIBRARY
    VERSION_VAR jemalloc_VERSION)
  mark_as_advanced(jemalloc_INCLUDE_DIRS jemalloc_LIBRARY jemalloc_VERSION
                   jemalloc_FOUND)
endmacro()

if(NOT jemalloc_ROOT)
  set(jemalloc_ROOT $ENV{jemalloc_ROOT})
endif()

# First check if jemalloc_config.sh is available
unset(_jemalloc_CONFIG_PATH)
set(jemalloc_FOUND FALSE)
find_program(
  _jemalloc_CONFIG_PATH
  NAMES jemalloc-config
  HINTS ${jemalloc_ROOT})

if(_jemalloc_CONFIG_PATH)
  _configure_use_jemalloc_config()
  if(jemalloc_FOUND)
    find_package_message(
      jemalloc "Found jemalloc by jemalloc.config: ${jemalloc_LIBRARY}"
      "[${jemalloc_LIBRARY}][${jemalloc_INCLUDE_DIRS}]")
    _configure_jemalloc_target()
    if(jemalloc_pic_LIBRARY)
      _configure_jemalloc_pic_target()
    endif()
    _finalize_find_package_jemalloc()
    return()
  endif()
endif()

# Manual find jemalloc by hand
find_path(
  jemalloc_INCLUDE_DIRS
  NAMES jemalloc/jemalloc.h
  PATH_SUFFIXES jemalloc jemalloc/include
  HINTS ${jemalloc_ROOT})
if(NOT jemalloc_INCLUDE_DIRS)
  _finalize_find_package_jemalloc()
  return()
endif()

_get_version()

find_library(
  jemalloc_LIBRARY
  NAMES libjemalloc.a
  HINTS ${jemalloc_ROOT})
if(jemalloc_LIBRARY)
  set(jemalloc_FOUND TRUE)
  _configure_jemalloc_target()
endif()
find_library(
  jemalloc_pic_LIBRARY
  NAMES libjemalloc_pic.a
  HINTS ${jemalloc_ROOT})
if(jemalloc_pic_LIBRARY)
  _configure_jemalloc_pic_target()
endif()

_finalize_find_package_jemalloc()
