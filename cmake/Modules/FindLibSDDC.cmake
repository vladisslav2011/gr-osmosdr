if(NOT LIBSDDC_FOUND)
  pkg_check_modules (LIBSDDC_PKG libsddc)
  message(STATUS "libsddc path: ${LIBSDDC_DIRS}")
  find_path(LIBSDDC_INCLUDE_DIRS NAMES libsddc.h
    PATHS
    ${LIBSDDC_PKG_INCLUDE_DIRS}
    ${LIBSDDC_DIRS}
    /usr/include
    /usr/local/include
  )

  find_library(LIBSDDC_LIBRARIES NAMES sddc
    PATHS
    ${LIBSDDC_PKG_LIBRARY_DIRS}
    ${LIBSDDC_DIRS}
    /usr/lib
    /usr/local/lib
  )

if(LIBSDDC_INCLUDE_DIRS AND LIBSDDC_LIBRARIES)
  set(LIBSDDC_FOUND TRUE CACHE INTERNAL "libsddc found")
  message(STATUS "Found libsddc: ${LIBSDDC_INCLUDE_DIRS}, ${LIBSDDC_LIBRARIES}")
else(LIBSDDC_INCLUDE_DIRS AND LIBSDDC_LIBRARIES)
  set(LIBSDDC_FOUND FALSE CACHE INTERNAL "libsddc found")
  message(STATUS "libsddc not found.")
endif(LIBSDDC_INCLUDE_DIRS AND LIBSDDC_LIBRARIES)

mark_as_advanced(LIBSDDC_LIBRARIES LIBSDDC_INCLUDE_DIRS)

endif(NOT LIBSDDC_FOUND)
