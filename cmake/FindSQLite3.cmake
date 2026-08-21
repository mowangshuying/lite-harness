# 使用本地 sqlite3 amalgamation 替代系统安装的 SQLite3
# 供 sqlite_orm 的 find_package(SQLite3 REQUIRED) 使用

set(SQLite3_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/3rdparty/sqlite" CACHE PATH "" FORCE)
set(SQLite3_LIBRARY sqlite3 CACHE STRING "" FORCE)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(SQLite3
    REQUIRED_VARS SQLite3_LIBRARY SQLite3_INCLUDE_DIR
)

if(SQLite3_FOUND AND NOT TARGET SQLite::SQLite3)
    add_library(SQLite::SQLite3 ALIAS sqlite3)
endif()
