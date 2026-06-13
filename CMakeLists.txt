cmake_minimum_required(VERSION 3.25)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project(ADB)

include(ExternalProject)
ExternalProject_Add(boringssl
    GIT_REPOSITORY "https://github.com/google/boringssl.git"
    GIT_TAG "0.20251002.0"
    SOURCE_DIR "${PROJECT_BINARY_DIR}/external/boringssl"
    CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${PROJECT_BINARY_DIR}/lib/boringssl -DCMAKE_CXX_FLAGS="-fPIC" -DBUILD_SHARED_LIBS=1 -DBUILD_STATIC_LIBS=1
)
set(BORINGSSL_INCLUDE_DIRS ${PROJECT_BINARY_DIR}/lib/boringssl/include)
set(BORINGSSL_LIBRARIES
    ${PROJECT_BINARY_DIR}/lib/boringssl/lib/libssl.so
    ${PROJECT_BINARY_DIR}/lib/boringssl/lib/libcrypto.so
)

include(CheckFunctionExists)
find_package(PkgConfig)
check_function_exists(strlcpy HAVE_STRLCPY)

if (NOT HAVE_STRLCPY)
    pkg_check_modules(LIBBSD REQUIRED libbsd)
    if (LIBBSD_FOUND)
        set(NEED_LIBBSD TRUE)
    else()
        message(WARNING "strlcpy not found in standard library, and libbsd is not available. You may need to install libbsd-dev.")
    endif()
endif()

find_package(spdlog REQUIRED)
find_package(fmt REQUIRED)

add_compile_options(-g -O0 -Wall -Wextra -Wno-error -Wno-non-virtual-dtor -Wno-unknown-pragmas -Wno-attributes -Wno-unused-parameter -Wno-missing-field-initializers -Wvla -DADB_HOST=1 -D__linux__=1)

set(SOURCES
    adb/adb.cpp
    adb/adb_io.cpp
    adb/adb_listeners.cpp
    adb/adb_trace.cpp
    adb/adb_unique_fd.cpp
    adb/adb_utils.cpp
    adb/services.cpp
    adb/sockets.cpp
    adb/socket_spec.cpp
    adb/sysdeps/env.cpp
    adb/sysdeps/errno.cpp
    adb/transport.cpp
    adb/transport_fd.cpp
    adb/types.cpp
    adb/shell_service_protocol.cpp

    adb/sysdeps_unix.cpp
    adb/sysdeps/posix/network.cpp

    adb/fdevent/fdevent.cpp
    adb/fdevent/fdevent_epoll.cpp

    adb/crypto/key.cpp
    adb/crypto/rsa_2048_key.cpp
    adb/crypto/x509_generator.cpp

    adb/tls/adb_ca_list.cpp
    adb/tls/tls_connection.cpp

    adb/client/auth.cpp
    adb/client/transport_local.cpp
    
    adb/client/adb_client.cpp
    adb/client/bugreport.cpp
    adb/client/commandline.cpp
    adb/client/file_sync_client.cpp
    adb/client/console.cpp
    adb/client/adb_install.cpp
    adb/client/line_printer.cpp

    adb/client/incremental.cpp
    adb/client/incremental_server.cpp
    adb/client/incremental_utils.cpp

)
# find_package(OpenSSL REQUIRED)
find_package(Protobuf REQUIRED)
#find_package(PkgConfig REQUIRED)
#pkg_check_modules(LIBBSD REQUIRED libbsd)
#pkg_check_modules(LIBUSB REQUIRED libusb-1.0)
pkg_check_modules(BROTLI REQUIRED libbrotlicommon libbrotlienc libbrotlidec)
pkg_check_modules(LZ4 REQUIRED liblz4)
pkg_check_modules(ZSTD REQUIRED libzstd)
#include_directories(${LIBUSB_INCLUDE_DIRS})
#message("LIBUSB_INCLUDE_DIRS: ${LIBUSB_INCLUDE_DIRS}")
#message("LIBBSD_LIBRARIES: ${LIBBSD_LIBRARIES}")

# Генерация protobuf
# protobuf_generate_cpp(APK_PROTO_SRCS APK_PROTO_HDRS adb/fastdeploy/proto/ApkEntry.proto)
# Указываем пути
set(PROTO_SRC ${CMAKE_SOURCE_DIR}/adb/fastdeploy/proto/ApkEntry.proto)
set(OUT_PROTO_DIR ${CMAKE_BINARY_DIR}/fastdeploy/proto)
set(APK_PROTO_SRCS ${OUT_PROTO_DIR}/ApkEntry.pb.cc)
set(APK_PROTO_HDRS ${OUT_PROTO_DIR}/ApkEntry.pb.h)

# Создаём директорию для выходных файлов
file(MAKE_DIRECTORY ${OUT_PROTO_DIR})

# Генерация файлов с помощью protoc
add_custom_command(
    OUTPUT ${APK_PROTO_SRCS} ${APK_PROTO_HDRS}
    COMMAND ${Protobuf_PROTOC_EXECUTABLE}
        --cpp_out=${OUT_PROTO_DIR}
        --proto_path=${CMAKE_SOURCE_DIR}/adb/fastdeploy/proto
        ${PROTO_SRC}
    DEPENDS ${PROTO_SRC}
    COMMENT "Generating protobuf files for ApkEntry.proto"
)

protobuf_generate_cpp(KEY_TYPE_PROTO_SRCS KEY_TYPE_PROTO_HDRS adb/proto/key_type.proto)
protobuf_generate_cpp(DEVICES_PROTO DEVICES_PROTO_HDRS adb/proto/devices.proto)
protobuf_generate_cpp(APP APP_HDRS adb/proto/app_processes.proto)
# protobuf_generate_cpp(FASTDEPLOY FASTDEPLOY_H adb/fastdeploy/profo)
set(SOURCES ${SOURCES} 
    ${APK_PROTO_SRCS} 
    ${KEY_TYPE_PROTO_SRCS}
    ${DEVICES_PROTO}
    ${APP}
)
set(ADIRECT_SOURCES
    adb/adb.cpp
    adb/adb_io.cpp
    adb/adb_listeners.cpp
    adb/adb_trace.cpp
    adb/adb_unique_fd.cpp
    adb/adb_utils.cpp
    adb/services.cpp
    adb/sockets.cpp
    adb/socket_spec.cpp
    adb/sysdeps/env.cpp
    adb/sysdeps/errno.cpp
    adb/transport.cpp
    adb/transport_fd.cpp
    adb/types.cpp
    adb/shell_service_protocol.cpp

    adb/sysdeps_unix.cpp
    adb/sysdeps/posix/network.cpp

    adb/fdevent/fdevent.cpp
    adb/fdevent/fdevent_epoll.cpp

    adb/crypto/key.cpp
    adb/crypto/rsa_2048_key.cpp
    adb/crypto/x509_generator.cpp

    adb/tls/adb_ca_list.cpp
    adb/tls/tls_connection.cpp

    adb/client/auth.cpp
    adb/client/transport_local.cpp
    
    adb/client/adb_client.cpp
    # adb/client/bugreport.cpp
    # adb/client/commandline.cpp
    adb/lib/src/file_sync_client.cpp
    adb/lib/src/helpers.cpp
    adb/client/console.cpp
    # adb/client/adb_install.cpp
    adb/client/line_printer.cpp

    adb/client/incremental.cpp
    adb/client/incremental_server.cpp
    adb/client/incremental_utils.cpp

    ${APK_PROTO_SRCS} 
    ${KEY_TYPE_PROTO_SRCS}
    ${DEVICES_PROTO}
    ${APP}

    adb/lib/src/AdbDevice.cpp
    adb/lib/src/AdbManager.cpp
    adb/lib/src/AdbSession.cpp
    adb/lib/src/AdbFileSync.cpp
    adb/lib/src/adirect.cpp
)
set(SOURCES ${SOURCES}
    adb/client/main.cpp
)
add_executable(adirect ${ADIRECT_SOURCES})
target_include_directories(adirect PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/adb
    ${CMAKE_SOURCE_DIR}/adb/client
    ${CMAKE_SOURCE_DIR}/adb/crypto/include
    ${CMAKE_SOURCE_DIR}/adb/tls/include
    ${CMAKE_SOURCE_DIR}/adb/client
    ${CMAKE_SOURCE_DIR}/system/libbase/include
    ${CMAKE_SOURCE_DIR}/system/core/libcutils/include
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_BINARY_DIR}/lib/boringssl/include
    ${CMAKE_SOURCE_DIR}/system/core/diagnose_usb
    ${CMAKE_SOURCE_DIR}/third_party/libcrypto_utils/include
    ${CMAKE_SOURCE_DIR}/system/libziparchive/include
    ${CMAKE_SOURCE_DIR}/adb/lib/include
)
target_link_libraries(adirect PRIVATE
    ${BORINGSSL_LIBRARIES}
    diagnose_usb crypto ssl crypto_utils build cutils log ziparchive
    ${Protobuf_LIBRARIES}
    ${BROTLI_LIBRARIES}
    ${LZ4_LIBRARIES}
    ${ZSTD_LIBRARIES}
)

add_executable(adb ${SOURCES})

target_compile_definitions(adb PRIVATE ANDROID_BASE_UNIQUE_FD_DISABLE_IMPLICIT_CONVERSION=1)
target_include_directories(adb PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/adb
    ${CMAKE_SOURCE_DIR}/adb/client
    ${CMAKE_SOURCE_DIR}/adb/crypto/include
    ${CMAKE_SOURCE_DIR}/adb/tls/include
    ${CMAKE_SOURCE_DIR}/adb/client   # client/*.h
    ${CMAKE_SOURCE_DIR}/system/libbase/include
    ${CMAKE_SOURCE_DIR}/system/core/libcutils/include
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_BINARY_DIR}/lib/boringssl/include
#    ${LIBBSD_INCLUDE_DIRS}
)
add_subdirectory(libbuildversion build)
add_subdirectory(system/core/diagnose_usb diagnose_usb)
add_subdirectory(system/libbase base)
add_subdirectory(system/libziparchive ziparchive)
add_subdirectory(third_party/libcrypto_utils crypto_utils)

set(LIBCUTILS_SOURCES
system/core/libcutils/android_get_control_file.cpp
system/core/libcutils/android_reboot.cpp
# system/core/libcutils/ashmem-host.cpp
system/core/libcutils/canned_fs_config.cpp
system/core/libcutils/config_utils.cpp
system/core/libcutils/fs_config.cpp
system/core/libcutils/fs.cpp
system/core/libcutils/hashmap.cpp
system/core/libcutils/iosched_policy.cpp
system/core/libcutils/klog.cpp
system/core/libcutils/load_file.cpp
system/core/libcutils/multiuser.cpp
system/core/libcutils/native_handle.cpp
system/core/libcutils/partition_utils.cpp
system/core/libcutils/properties.cpp
system/core/libcutils/qtaguid.cpp
system/core/libcutils/record_stream.cpp
system/core/libcutils/socket_inaddr_any_server_unix.cpp
# system/core/libcutils/socket_inaddr_any_server_windows.cpp
system/core/libcutils/socket_local_client_unix.cpp
system/core/libcutils/socket_local_server_unix.cpp
system/core/libcutils/socket_network_client_unix.cpp
# system/core/libcutils/socket_network_client_windows.cpp
system/core/libcutils/sockets.cpp
system/core/libcutils/sockets_unix.cpp
# system/core/libcutils/sockets_windows.cpp
system/core/libcutils/str_parms.cpp
# system/core/libcutils/trace-container.cpp
# system/core/libcutils/trace-host.cpp
system/core/libcutils/uevent.cpp

)
# Создаем статическую библиотеку
add_library(cutils STATIC ${LIBCUTILS_SOURCES})

target_include_directories(cutils PRIVATE
    ${CMAKE_SOURCE_DIR}/system/libbase/include
    ${CMAKE_SOURCE_DIR}/system/liblog/include
    ${CMAKE_SOURCE_DIR}/system/core/libcutils/include
)
target_compile_definitions(cutils PRIVATE
    -DADB_HOST=1
    -D_GLIBCXX_USE_CXX11_ABI=1
)
target_link_libraries(cutils base)

add_subdirectory(system/liblog log)
add_subdirectory(system/core/libutils utils)

target_link_libraries(adb PRIVATE
    ${BORINGSSL_LIBRARIES}
    diagnose_usb crypto ssl crypto_utils build cutils log ziparchive
#    ${LIBUSB_LIBRARIES}
    ${Protobuf_LIBRARIES}
    ${BROTLI_LIBRARIES}
    ${LZ4_LIBRARIES}
    ${ZSTD_LIBRARIES}
#    ${LIBBSD_LIBRARIES}
)
target_include_directories(adb PRIVATE 
    ${CMAKE_SOURCE_DIR}/system/core/diagnose_usb
    ${CMAKE_SOURCE_DIR}/third_party/libcrypto_utils/include
    ${CMAKE_SOURCE_DIR}/system/libziparchive/include
)

if (NEED_LIBBSD)
    target_include_directories(adb PRIVATE ${LIBBSD_INCLUDE_DIRS})
    target_include_directories(adirect PRIVATE ${LIBBSD_INCLUDE_DIRS})
    target_link_libraries(adb PRIVATE ${LIBBSD_LIBRARIES})
    target_link_libraries(adirect PRIVATE ${LIBBSD_LIBRARIES})
endif()
target_link_libraries(adb PRIVATE spdlog::spdlog fmt::fmt)
target_link_libraries(adirect PRIVATE spdlog::spdlog fmt::fmt)

target_include_directories(ziparchive PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)


# Инсталляция adb
set(CMAKE_INSTALL_PREFIX /opt)
install(TARGETS adb
    RUNTIME DESTINATION adb
)

# Установка динамических библиотек
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/lib/boringssl/lib/libssl.so
    ${CMAKE_CURRENT_BINARY_DIR}/lib/boringssl/lib/libcrypto.so
    ${CMAKE_CURRENT_BINARY_DIR}/ziparchive/libziparchive.so
    DESTINATION adb/lib
)

# Настройка RPATH для автоматического поиска библиотек
set_target_properties(adb PROPERTIES
    BUILD_RPATH "$ORIGIN/lib"
    INSTALL_RPATH "$ORIGIN/lib"
)
