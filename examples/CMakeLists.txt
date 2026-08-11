cmake_minimum_required(VERSION 3.25)
project(libadb_examples CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Находим libadb двумя способами:
#   1) из установленного пакета:   cmake -S examples -B build-ex
#   2) из дерева исходников напрямую (libadb::adb уже определён как alias):
#      cmake -S . -B build && cmake --build build (примеры собираются вместе)
if (NOT TARGET libadb::adb)
    find_package(libadb 1.0 REQUIRED)
endif()

# Все примеры линкуются одинаково. rpath $ORIGIN позволяет запускать их без
# установки библиотеки в систему (достаточно лежащей рядом libadb.so.1).
function(add_example name src)
    add_executable(${name} ${src})
    target_link_libraries(${name} PRIVATE libadb::adb)
    set_target_properties(${name} PROPERTIES
        INSTALL_RPATH "$ORIGIN/../lib"
        BUILD_RPATH   "$ORIGIN/../lib"
    )
endfunction()

add_subdirectory(cpp)
add_subdirectory(c)
