cmake_minimum_required(VERSION 3.16)
project(ncvista VERSION 1.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Install prefix for netCDF and udunits2. If not given on the command line
# (-DHPC_PREFIX=/path/to/prefix) or via the HPC_PREFIX environment variable, it
# is auto-detected from nc-config. Leave empty to use the system default paths.
set(_ncv_default_prefix "")
if(DEFINED ENV{HPC_PREFIX})
  set(_ncv_default_prefix "$ENV{HPC_PREFIX}")
else()
  # Resolve nc-config via PATH (matches the shell) and use its prefix.
  execute_process(COMMAND nc-config --prefix
                  OUTPUT_VARIABLE _ncv_default_prefix
                  OUTPUT_STRIP_TRAILING_WHITESPACE
                  ERROR_QUIET RESULT_VARIABLE _ncv_rc)
  if(NOT _ncv_rc EQUAL 0)
    set(_ncv_default_prefix "")
  endif()
endif()
set(HPC_PREFIX "${_ncv_default_prefix}" CACHE PATH "netCDF/udunits install prefix")

find_package(PkgConfig REQUIRED)
pkg_check_modules(GUI REQUIRED x11 cairo cairo-xlib pangocairo)
# The site fontconfig (pulled in by cairo) needs a specific libxml2 version;
# resolve it explicitly so the linker matches fontconfig's versioned symbols.
pkg_check_modules(XML2 libxml-2.0)

add_executable(ncvista
  src/app.cpp
  src/ncfile.cpp
  src/units.cpp
  src/colormap.cpp
  src/overlay.cpp)

target_include_directories(ncvista PRIVATE ${GUI_INCLUDE_DIRS})
if(HPC_PREFIX)
  target_include_directories(ncvista PRIVATE ${HPC_PREFIX}/include)
endif()

# Coastline / border data file locations (resolved at runtime, in order, after
# the env overrides and paths relative to the executable).
target_compile_definitions(ncvista PRIVATE
  NCVISTA_VERSION="${PROJECT_VERSION}"
  NCVISTA_COAST_SRC="${CMAKE_CURRENT_SOURCE_DIR}/coastlines.bin"
  NCVISTA_COAST_INSTALL="${CMAKE_INSTALL_PREFIX}/share/ncvista/coastlines.bin"
  NCVISTA_BORDERS_SRC="${CMAKE_CURRENT_SOURCE_DIR}/borders.bin"
  NCVISTA_BORDERS_INSTALL="${CMAKE_INSTALL_PREFIX}/share/ncvista/borders.bin")

target_link_directories(ncvista PRIVATE
  ${GUI_LIBRARY_DIRS}
  ${XML2_LIBRARY_DIRS})
if(HPC_PREFIX)
  target_link_directories(ncvista PRIVATE ${HPC_PREFIX}/lib)
endif()

target_compile_options(ncvista PRIVATE ${GUI_CFLAGS_OTHER} -Wall -Wextra)

target_link_libraries(ncvista PRIVATE
  ${GUI_LIBRARIES}
  ${XML2_LIBRARIES}
  netcdf
  udunits2)

# Bake rpaths so the binary finds the netCDF/udunits libraries at runtime.
set(_ncv_rpath "${XML2_LIBRARY_DIRS}")
if(HPC_PREFIX)
  list(PREPEND _ncv_rpath "${HPC_PREFIX}/lib")
endif()
set_target_properties(ncvista PROPERTIES
  BUILD_RPATH "${_ncv_rpath}"
  INSTALL_RPATH "${_ncv_rpath}")

install(TARGETS ncvista RUNTIME DESTINATION bin)
install(FILES coastlines.bin borders.bin DESTINATION share/ncvista OPTIONAL)
