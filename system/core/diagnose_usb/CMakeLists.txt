add_library(diagnose_usb STATIC diagnose_usb.cpp)
target_link_libraries(diagnose_usb base)  # Зависимость от libbase
target_include_directories(diagnose_usb PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/system/libbase/include
)