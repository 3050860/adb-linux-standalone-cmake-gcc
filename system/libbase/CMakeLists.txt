add_compile_options(-D_FILE_OFFSET_BITS=64)

set(SOURCES
        abi_compatibility.cpp
        chrono_utils.cpp
        cmsg.cpp
        file.cpp
        hex.cpp
        logging.cpp
        mapped_file.cpp
        parsebool.cpp
        parsenetaddress.cpp
        posix_strerror_r.cpp
        process.cpp
        properties.cpp
        result.cpp
        stringprintf.cpp
        strings.cpp
        threads.cpp
        test_utils.cpp

        errors_unix.cpp

)

add_library(base STATIC ${SOURCES})
target_link_libraries(diagnose_usb base)  # Зависимость от libbase
target_include_directories(base PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/system/libbase/include
    ${CMAKE_SOURCE_DIR}/system/liblog/include
)
# target_compile_definitions(base PRIVATE
#     __builtin_available=0  # Отключает проверки __builtin_available
# )
target_compile_options(base PUBLIC -fPIC)