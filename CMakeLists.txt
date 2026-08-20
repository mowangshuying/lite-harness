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
    "src/App.cpp"
    "src/BasePage.cpp"
    "src/BasePage.h"
    "src/ChatMsgEdit.cpp"
    "src/ChatMsgEdit.h"
    "src/LiteHarness.cpp"
    "src/LiteHarness.h"
    "src/SendMsgButton.cpp"
    "src/SendMsgButton.h"
    "src/SettingsPage.cpp"
    "src/SettingsPage.h"
    "src/NewChatPage.cpp"
    "src/NewChatPage.h"
    )

set (qss
        "./stylesheet/light/BasePage.qss"
        "./stylesheet/dark/BasePage.qss"
        "./stylesheet/atomOneDark/BasePage.qss"
        "./stylesheet/light/ChatMsgEdit.qss"
        "./stylesheet/dark/ChatMsgEdit.qss"
        "./stylesheet/atomOneDark/ChatMsgEdit.qss"
        "./stylesheet/light/SendMsgButton.qss"
        "./stylesheet/dark/SendMsgButton.qss"
        "./stylesheet/atomOneDark/SendMsgButton.qss"
        "./stylesheet/light/LiteHarness.qss"
        "./stylesheet/dark/LiteHarness.qss"
        "./stylesheet/atomOneDark/LiteHarness.qss"
        "./stylesheet/light/SettingsPage.qss"
        "./stylesheet/dark/SettingsPage.qss"
        "./stylesheet/atomOneDark/SettingsPage.qss"
        "./stylesheet/light/NewChatPage.qss"
        "./stylesheet/dark/NewChatPage.qss"
        "./stylesheet/atomOneDark/NewChatPage.qss"
)
source_group(TREE ${CMAKE_SOURCE_DIR} FILES ${qss})

set(res
        "./res/LiteHarness.ico"
)
source_group(TREE ${CMAKE_SOURCE_DIR} FILES ${res})

set(TGT lite-harness)
add_executable(${TGT} ${src} ${inc} "${CMAKE_CURRENT_SOURCE_DIR}/LiteHarness.rc")

### qss
qt_add_resources(${TGT} "qss" PREFIX "/" FILES ${qss})

### icon
qt_add_resources(${TGT} "res" PREFIX "/" FILES ${res})

target_link_libraries(${TGT} PRIVATE 
    FluentUI::Controls
    FluentUI::Utils
)