cmake_minimum_required(VERSION 3.20)
project(sqlite3 LANGUAGES C)

add_library(sqlite3 STATIC sqlite3.c)

target_include_directories(sqlite3 PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

# 提供与 find_package(SQLite3) 一致的导入目标，供 sqlite_orm 使用
add_library(SQLite::SQLite3 ALIAS sqlite3)

# sqlite_orm 的 install(EXPORT) 要求依赖目标也在 export set 中
install(TARGETS sqlite3 EXPORT SqliteOrmTargets)
