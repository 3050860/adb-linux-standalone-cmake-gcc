add_library(crypto_utils STATIC android_pubkey.cpp)
# Без этой зависимости чистая сборка — гонка: android_pubkey.cpp использует
# BN_le2bn/BN_bn2le_padded, которых нет в системном OpenSSL, и компилировался
# раньше, чем ExternalProject разложит заголовки BoringSSL.
add_dependencies(crypto_utils boringssl)
# target_link_libraries(diagnose_usb base)  # Зависимость от libbase
# target_compile_definitions(crypto_utils PRIVATE OPENSSL_NO_SYSTEM)
target_include_directories(crypto_utils PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PROJECT_BINARY_DIR}/lib/boringssl/include
)