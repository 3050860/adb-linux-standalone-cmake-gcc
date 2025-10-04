add_library(crypto_utils STATIC android_pubkey.cpp)
# target_link_libraries(diagnose_usb base)  # Зависимость от libbase
# target_compile_definitions(crypto_utils PRIVATE OPENSSL_NO_SYSTEM)
target_include_directories(crypto_utils PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/boringssl/src/include
)