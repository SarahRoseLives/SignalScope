#----------------------------------------------------------------
# Generated CMake target import file for configuration "RelWithDebInfo".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "SoapySDR" for configuration "RelWithDebInfo"
set_property(TARGET SoapySDR APPEND PROPERTY IMPORTED_CONFIGURATIONS RELWITHDEBINFO)
set_target_properties(SoapySDR PROPERTIES
  IMPORTED_IMPLIB_RELWITHDEBINFO "${_IMPORT_PREFIX}/lib/SoapySDR.lib"
  IMPORTED_LOCATION_RELWITHDEBINFO "${_IMPORT_PREFIX}/bin/SoapySDR.dll"
  )

list(APPEND _IMPORT_CHECK_TARGETS SoapySDR )
list(APPEND _IMPORT_CHECK_FILES_FOR_SoapySDR "${_IMPORT_PREFIX}/lib/SoapySDR.lib" "${_IMPORT_PREFIX}/bin/SoapySDR.dll" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
