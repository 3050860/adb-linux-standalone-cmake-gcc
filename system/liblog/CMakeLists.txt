
add_library(log STATIC 
    log_event_list.cpp
    log_event_write.cpp
    logger_name.cpp
    logger_read.cpp
    logger_write.cpp
    logprint.cpp
    properties.cpp
    
    event_tag_map.cpp
    log_time.cpp
    pmsg_reader.cpp
    pmsg_writer.cpp
    logd_reader.cpp
    logd_writer.cpp
    
)
# target_link_libraries(ziparchive base)  # Зависимость от libbase
target_include_directories(log PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/system/core/libcutils/include
    # ${CMAKE_CURRENT_SOURCE_DIR}/incfs_support/include
    ${CMAKE_SOURCE_DIR}/system/libbase/include
    ${CMAKE_SOURCE_DIR}/system/core/libutils/include
)
target_compile_options(log PRIVATE -fPIC )