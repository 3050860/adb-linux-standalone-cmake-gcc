# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/home/nk/AndroidStudioProjects/adb-linux-standalone-cmake-gcc/adb/external/boringssl"
  "/home/nk/AndroidStudioProjects/adb-linux-standalone-cmake-gcc/adb/boringssl-prefix/src/boringssl-build"
  "/home/nk/AndroidStudioProjects/adb-linux-standalone-cmake-gcc/adb/boringssl-prefix"
  "/home/nk/AndroidStudioProjects/adb-linux-standalone-cmake-gcc/adb/boringssl-prefix/tmp"
  "/home/nk/AndroidStudioProjects/adb-linux-standalone-cmake-gcc/adb/boringssl-prefix/src/boringssl-stamp"
  "/home/nk/AndroidStudioProjects/adb-linux-standalone-cmake-gcc/adb/boringssl-prefix/src"
  "/home/nk/AndroidStudioProjects/adb-linux-standalone-cmake-gcc/adb/boringssl-prefix/src/boringssl-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/nk/AndroidStudioProjects/adb-linux-standalone-cmake-gcc/adb/boringssl-prefix/src/boringssl-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/nk/AndroidStudioProjects/adb-linux-standalone-cmake-gcc/adb/boringssl-prefix/src/boringssl-stamp${cfgdir}") # cfgdir has leading slash
endif()
