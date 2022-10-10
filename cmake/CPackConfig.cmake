# RPM specifics
if(CPACK_GENERATOR MATCHES "RPM")
  set(CPACK_PACKAGING_INSTALL_PREFIX "/")
  set(CPACK_COMPONENTS_ALL clients-el8 server-el8 clients-versioned server-versioned)
elseif(CPACK_GENERATOR MATCHES "DEB")
  set(CPACK_PACKAGING_INSTALL_PREFIX "/")
  set(CPACK_COMPONENTS_ALL clients-deb server-deb clients-versioned server-versioned)
elseif(CPACK_GENERATOR MATCHES "TGZ")
  set(CPACK_COMPONENTS_ALL clients-tgz server-tgz)
else()
  message(FATAL_ERROR "Unsupported package format ${CPACK_GENERATOR}")
endif()

if(${FDB_RELEASE} STREQUAL "ON")
  message("Strip binaries for release")
  set(CPACK_STRIP_FILES TRUE)
endif()

set(CPACK_RESOURCE_FILE_README ${CMAKE_SOURCE_DIR}/README.md)
set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_SOURCE_DIR}/LICENSE)
