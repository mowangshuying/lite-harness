cmake_minimum_required(VERSION 3.20)
project(lite-harness VERSION 0.0.1)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG "${PROJECT_BINARY_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${PROJECT_BINARY_DIR}/bin")

# find_package(QT NAMES Qt5 Qt6 REQUIRED COMPONENTS Core)
find_package(QT NAMES Qt6 REQUIRED COMPONENTS Widgets Svg Charts Core5Compat Xml)
find_package(Qt6 REQUIRED COMPONENTS Widgets Svg Charts Core5Compat Xml)

if (MSVC_IDE)
	message(STATUS "Qt6Widgets_LIBRARIES: ${Qt6Widgets_LIBRARIES}")
	get_target_property(V_LOCATION ${Qt6Widgets_LIBRARIES} IMPORTED_LOCATION)
	message(STATUS "V_LOCATION: ${V_LOCATION}")
	cmake_path(GET V_LOCATION PARENT_PATH V_RUN_PATH)
	message(STATUS "V_RUN_PATH: ${V_RUN_PATH}")
	set(CMAKE_VS_DEBUGGER_ENVIRONMENT "PATH=%PATH%;${V_RUN_PATH}")
	message(STATUS "CMAKE_VS_DEBUGGER_ENVIRONMENT: ${CMAKE_VS_DEBUGGER_ENVIRONMENT}")
endif()

# add_subdirectory(src)
add_subdirectory(3rdparty/FluentUI)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/FluentUI/controls
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/FluentUI/utils
)

set(inc "")
set(src 
    "src/App.cpp")

set(TGT lite-harness)
add_executable(${TGT} ${src} ${inc})
target_link_libraries(${TGT} PRIVATE 
    FluentUI::Controls
    FluentUI::Utils
)