cmake_minimum_required(VERSION 3.20)
# 版本号与 tag v0.1.0 对齐（原 0.0.1 与实际发布版本脱节）
project(lite-harness VERSION 0.1.0)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG "${PROJECT_BINARY_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${PROJECT_BINARY_DIR}/bin")

# 自定义 Find 模块路径
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# 组件裁剪：本工程实际仅使用 Widgets / Svg(FluentUI 需要) / Network(LLM 请求)
# Charts、Core5Compat 由 FluentUI 子构建自行 find_package，根处无需重复申请；Xml 无任何引用
# 已删除注释掉的 Qt5 探测行（历史死代码）
find_package(QT NAMES Qt6 REQUIRED COMPONENTS Widgets Svg Network)
find_package(Qt6 REQUIRED COMPONENTS Widgets Svg Network)

# MSVC IDE 调试环境：将 Qt bin 目录注入 VS 调试器 PATH，保证 F5 调试可加载 DLL
# 已删除中间变量的 message(STATUS) 噪音输出，仅保留最终结果提示
if (MSVC_IDE)
	get_target_property(V_LOCATION ${Qt6Widgets_LIBRARIES} IMPORTED_LOCATION)
	cmake_path(GET V_LOCATION PARENT_PATH V_RUN_PATH)
	set(CMAKE_VS_DEBUGGER_ENVIRONMENT "PATH=%PATH%;${V_RUN_PATH}")
	message(STATUS "CMAKE_VS_DEBUGGER_ENVIRONMENT: ${CMAKE_VS_DEBUGGER_ENVIRONMENT}")
endif()

# add_subdirectory(src)
add_subdirectory(3rdparty/FluentUI)
# 摘除 sqlite / sqlite_orm 子构建：全仓零引用（src/ 与 FluentUI 均经 grep 核实），
# 免去约 25 万行 C 死重编译；3rdparty 目录保留，仅移出构建图
# 原全局 include_directories 一并移除：sqlite_orm/include 随死依赖删除，
# FluentUI controls/utils 两行下方改为 target_include_directories（作用域收敛到本目标）

# 源文件列表自动化：CONFIGURE_DEPENDS 在构建时检测文件增删并自动重新 configure，
# 取代手工枚举（易漏加新文件）；路径相对 CMAKE_CURRENT_SOURCE_DIR（仓库根）
file(GLOB src CONFIGURE_DEPENDS "src/*.cpp" "src/*.h")

# QSS 主题样式表自动化（light/dark/atomOneDark 三主题，原手工枚举 36 项），理由同上
# 注意：file(GLOB) 返回绝对路径，而 qt_add_resources 对绝对路径要求显式 QT_RESOURCE_ALIAS；
# 这里统一转回相对仓库根路径，与原手工清单 "./stylesheet/..." 形态一致，
# 保证 QRC 别名仍为 /stylesheet/<theme>/<name>.qss，运行时 FluStyleSheetUtils 路径解析不受影响
file(GLOB_RECURSE qss CONFIGURE_DEPENDS "stylesheet/*.qss")
set(qss_rel "")
foreach(f ${qss})
    file(RELATIVE_PATH rf ${CMAKE_CURRENT_SOURCE_DIR} ${f})
    list(APPEND qss_rel "./${rf}")
endforeach()
source_group(TREE ${CMAKE_SOURCE_DIR} FILES ${qss})

set(res
        "./res/LiteHarness.ico"
)
source_group(TREE ${CMAKE_SOURCE_DIR} FILES ${res})

set(TGT lite-harness)
# 原 ${inc} 引用随空的 set(inc "") 一并删除（从未有内容，死变量）
add_executable(${TGT} ${src} "${CMAKE_CURRENT_SOURCE_DIR}/LiteHarness.rc")

# FluentUI 头文件搜索路径：由全局 include_directories 迁移为仅作用于本目标，
# 避免污染后续 add_subdirectory 的构建（等价性：原全局块位于 FluentUI 子构建之后，仅本可执行消费）
target_include_directories(${TGT} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/FluentUI/controls
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/FluentUI/utils
)

# MSVC：/W4 提升警告可见性；/utf-8 统一源码与执行字符集（避免中文注释按系统代码页误读）
# 不加 /WX：第三方/存量警告不致中断构建
target_compile_options(${TGT} PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/W4 /utf-8>)

### qss
qt_add_resources(${TGT} "qss" PREFIX "/" FILES ${qss_rel})

### icon
qt_add_resources(${TGT} "res" PREFIX "/" FILES ${res})

target_link_libraries(${TGT} PRIVATE 
    FluentUI::Controls
    FluentUI::Utils
    Qt6::Network
)

# ---- windeployqt 部署（可选目标，不进 ALL，手动触发：cmake --build build --config Release --target deploy）----
# -DLITE_DEPLOY=OFF 整体跳过。windeployqt 定位三级：${CMAKE_PREFIX_PATH}/bin（configure 传 -D 时生效）
# → Qt6::qmake 导入目标所在 bin（本仓 build 缓存无 CMAKE_PREFIX_PATH 条目，find_package 缓存的
# Qt6_DIR 同样可推导出 Qt 安装目录，此级为可靠主力）→ find_program 兜底。找不到则只告警不建目标。
# 不传 --compiled：windeployqt 6.9 已移除该选项（实测 "Unknown option 'compiled'."，exit 1）。
option(LITE_DEPLOY "Create an optional 'deploy' target running windeployqt on the executable" ON)
if (LITE_DEPLOY)
	set(WINDEPLOYQT_EXE "")
	foreach(prefix ${CMAKE_PREFIX_PATH})
		if (EXISTS "${prefix}/bin/windeployqt.exe")
			set(WINDEPLOYQT_EXE "${prefix}/bin/windeployqt.exe")
			break()
		endif()
	endforeach()
	if (NOT WINDEPLOYQT_EXE AND TARGET Qt6::qmake)
		get_target_property(_qmake_exe Qt6::qmake IMPORTED_LOCATION)
		cmake_path(GET _qmake_exe PARENT_PATH _qt_bin_dir)
		if (EXISTS "${_qt_bin_dir}/windeployqt.exe")
			set(WINDEPLOYQT_EXE "${_qt_bin_dir}/windeployqt.exe")
		endif()
	endif()
	if (NOT WINDEPLOYQT_EXE)
		find_program(WINDEPLOYQT_EXE NAMES windeployqt windeployqt.exe)
	endif()
	if (WINDEPLOYQT_EXE)
		# DEPENDS 保证 exe 已构建；$<TARGET_FILE> 随 --config 解析到对应配置的输出
		add_custom_target(deploy
			COMMAND "${WINDEPLOYQT_EXE}" --no-translations --no-compiler-runtime $<TARGET_FILE:${TGT}>
			DEPENDS ${TGT}
			COMMENT "windeployqt -> $<TARGET_FILE:${TGT}>")
	else()
		message(WARNING "windeployqt not found; 'deploy' target is not available")
	endif()
endif()
