
add_library(ziparchive SHARED 
        zip_archive.cc
        zip_archive_stream_entry.cc
        zip_cd_entry_map.cc
        zip_error.cpp
        zip_writer.cc
)
target_link_libraries(ziparchive base log)  # Зависимость от libbase
target_include_directories(ziparchive PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/incfs_support/include
    ${CMAKE_SOURCE_DIR}/system/libbase/include
    ${CMAKE_SOURCE_DIR}/system/liblog/include
)
target_compile_options(ziparchive PRIVATE -fpermissive )