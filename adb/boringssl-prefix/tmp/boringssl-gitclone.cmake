# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

if(EXISTS "/home/nk/AndroidStudioProjects/adb-linux-standalone-cmake-gcc/adb/boringssl-prefix/src/boringssl-stamp/boringssl-gitclone-lastrun.txt" AND EXISTS "/home/nk/AndroidStudioProjects/adb-linux-standalone-cmake-gcc/adb/boringssl-prefix/src/boringssl-stamp/boringssl-gitinfo.txt" AND
  "/home/nk/AndroidStudioProjects/adb-linux-standalone-cmake-gcc/adb/boringssl-prefix/src/boringssl-stamp/boringssl-gitclone-lastrun.txt" IS_NEWER_THAN "/home/nk/AndroidStudioProjects/adb-linux-standalone-cmake-gcc/adb/boringssl-prefix/src/boringssl-stamp/boringssl-gitinfo.txt")
  message(STATUS
    "Avoiding repeated git clone, stamp file is up to date: "
    "'/home/nk/AndroidStudioProjects/adb-linux-standalone-cmake-gcc/adb/boringssl-prefix/src/boringssl-stamp/boringssl-gitclone-lastrun.txt'"
  )
  return()
endif()

execute_process(
  COMMAND ${CMAKE_COMMAND} -E rm -rf "/home/nk/AndroidStudioProjects/adb-linux-standalone-cmake-gcc/adb/external/boringssl"
  RESULT_VARIABLE error_code
)
if(error_code)
  message(FATAL_ERROR "Failed to remove directory: '/home/nk/AndroidStudioProjects/adb-linux-standalone-cmake-gcc/adb/external/boringssl'")
endif()

# try the clone 3 times in case there is an odd git clone issue
set(error_code 1)
set(number_of_tries 0)
while(error_code AND number_of_tries LESS 3)
  execute_process(
    COMMAND "/usr/bin/git"
            clone --no-checkout --config "advice.detachedHead=false" "https://github.com/google/boringssl.git" "boringssl"
    WORKING_DIRECTORY "/home/nk/AndroidStudioProjects/adb-linux-standalone-cmake-gcc/adb/external"
    RESULT_VARIABLE error_code
  )
  math(EXPR number_of_tries "${number_of_tries} + 1")
endwhile()
if(number_of_tries GREATER 1)
  message(STATUS "Had to git clone more than once: ${number_of_tries} times.")
endif()
if(error_code)
  message(FATAL_ERROR "Failed to clone repository: 'https://github.com/google/boringssl.git'")
endif()

execute_process(
  COMMAND "/usr/bin/git"
          checkout "0.20251002.0" --
  WORKING_DIRECTORY "/home/nk/AndroidStudioProjects/adb-linux-standalone-cmake-gcc/adb/external/boringssl"
  RESULT_VARIABLE error_code
)
if(error_code)
  message(FATAL_ERROR "Failed to checkout tag: '0.20251002.0'")
endif()

set(init_submodules TRUE)
if(init_submodules)
  execute_process(
    COMMAND "/usr/bin/git" 
            submodule update --recursive --init 
    WORKING_DIRECTORY "/home/nk/AndroidStudioProjects/adb-linux-standalone-cmake-gcc/adb/external/boringssl"
    RESULT_VARIABLE error_code
  )
endif()
if(error_code)
  message(FATAL_ERROR "Failed to update submodules in: '/home/nk/AndroidStudioProjects/adb-linux-standalone-cmake-gcc/adb/external/boringssl'")
endif()

# Complete success, update the script-last-run stamp file:
#
execute_process(
  COMMAND ${CMAKE_COMMAND} -E copy "/home/nk/AndroidStudioProjects/adb-linux-standalone-cmake-gcc/adb/boringssl-prefix/src/boringssl-stamp/boringssl-gitinfo.txt" "/home/nk/AndroidStudioProjects/adb-linux-standalone-cmake-gcc/adb/boringssl-prefix/src/boringssl-stamp/boringssl-gitclone-lastrun.txt"
  RESULT_VARIABLE error_code
)
if(error_code)
  message(FATAL_ERROR "Failed to copy script-last-run stamp file: '/home/nk/AndroidStudioProjects/adb-linux-standalone-cmake-gcc/adb/boringssl-prefix/src/boringssl-stamp/boringssl-gitclone-lastrun.txt'")
endif()
