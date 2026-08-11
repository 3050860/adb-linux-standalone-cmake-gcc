# STATIC (этап 12): как разделяемая библиотека этот таргет был непригоден —
# собирался вообще без SONAME, то есть установить его в систему нельзя. Теперь
# линкуется внутрь libadb.so, наружу символы не видны (version script).
# POSITION_INDEPENDENT_CODE обязателен: объекты попадают в .so.
add_library(ziparchive STATIC
        zip_archive.cc
        zip_archive_stream_entry.cc
        zip_cd_entry_map.cc
        zip_error.cpp
        zip_writer.cc
)
set_target_properties(ziparchive PROPERTIES POSITION_INDEPENDENT_CODE ON)
# z указан явно и PUBLIC: раньше zlib подтягивался неявно, как зависимость
# разделяемой libziparchive.so. У статической библиотеки своих зависимостей нет,
# поэтому zlib обязан попасть в командную строку каждого, кто её линкует
# (иначе "undefined reference to inflateEnd: DSO missing from command line").
find_package(ZLIB REQUIRED)
target_link_libraries(ziparchive PUBLIC base log ZLIB::ZLIB)
target_include_directories(ziparchive PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/incfs_support/include
    ${CMAKE_SOURCE_DIR}/system/libbase/include
    ${CMAKE_SOURCE_DIR}/system/liblog/include
)
target_compile_options(ziparchive PRIVATE -Wno-sign-compare -Wno-conversion -fpermissive)
