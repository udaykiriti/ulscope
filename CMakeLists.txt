cmake_minimum_required(VERSION 3.20)
project(ulpscope VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MPFR REQUIRED IMPORTED_TARGET mpfr)

# ---------------------------------------------------------------- core library
# UI-free numeric core: formats, MPFR reference, dynamic loader, scanner.
# The Qt-dependent number formatting it reuses is listed explicitly, so the core
# still needs nothing beyond QtCore/Gui.
add_library(ulpscope_core STATIC
  src/core/format.cpp
  src/core/mpfrref.cpp
  src/core/dynlib.cpp
  src/core/scan.cpp
  src/core/sweep.cpp
  src/core/exporter.cpp
)
target_include_directories(ulpscope_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(ulpscope_core
  PUBLIC PkgConfig::MPFR
  PRIVATE Qt6::Core ${CMAKE_DL_LIBS}
)

# ---------------------------------------------------------------- demo library
# Stand-in for "an llvm-libc build". Contains both a correct implementation and
# deliberately mis-rounded ones so the ULP machinery has something to find.
add_library(ulpscope_demo SHARED demo/bf16_demo.cpp)
set_target_properties(ulpscope_demo PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  POSITION_INDEPENDENT_CODE ON
)

# ---------------------------------------------------------------- application
qt_add_executable(ulpscope
  src/main.cpp
  src/ui/mainwindow.cpp
  src/ui/bitinspector.cpp
  src/ui/ulpplot.cpp
  src/ui/worsttable.cpp
  src/ui/sweeptable.cpp
)
target_link_libraries(ulpscope PRIVATE ulpscope_core Qt6::Widgets)
target_compile_definitions(ulpscope PRIVATE
  ULPSCOPE_DEMO_SO="$<TARGET_FILE:ulpscope_demo>"
)
add_dependencies(ulpscope ulpscope_demo)

# ---------------------------------------------------------------- unit tests
enable_testing()
add_executable(ulpscope_tests tests/test_main.cpp)
target_link_libraries(ulpscope_tests PRIVATE ulpscope_core Qt6::Core Qt6::Gui)
target_compile_definitions(ulpscope_tests PRIVATE
  ULPSCOPE_DEMO_SO="$<TARGET_FILE:ulpscope_demo>"
)
add_dependencies(ulpscope_tests ulpscope_demo)
add_test(NAME ulpscope_tests COMMAND ulpscope_tests)

# Throwaway paint benchmark, kept out of the default target list.
add_executable(ulpscope_bench EXCLUDE_FROM_ALL bench/plotbench.cpp src/ui/ulpplot.cpp)
target_link_libraries(ulpscope_bench PRIVATE ulpscope_core Qt6::Widgets)
target_compile_definitions(ulpscope_bench PRIVATE
  ULPSCOPE_DEMO_SO="$<TARGET_FILE:ulpscope_demo>")
add_dependencies(ulpscope_bench ulpscope_demo)

install(TARGETS ulpscope RUNTIME DESTINATION bin)