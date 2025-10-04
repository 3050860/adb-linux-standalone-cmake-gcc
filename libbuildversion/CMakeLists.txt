add_library(build STATIC libbuildversion.cpp)
# target_link_libraries(diagnose_usb base)  # Зависимость от libbase
target_include_directories(build PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    # ${CMAKE_SOURCE_DIR}/system/libbase/include
)