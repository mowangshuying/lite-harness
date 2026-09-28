cmake_minimum_required(VERSION 3.20)
# 版本号与基线 tag s00 对齐（s00 前身即 v0.1.0；原 0.0.1 与实际发布版本脱节）
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
# LinguistTools（i18n 第八轮）：提供 Qt6::lupdate/lrelease 导入目标，供下方 qt_add_translations
find_package(QT NAMES Qt6 REQUIRED COMPONENTS Widgets Svg Network LinguistTools)
find_package(Qt6 REQUIRED COMPONENTS Widgets Svg Network LinguistTools)

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

# 版本单单源：CMake project VERSION → LITE_VERSION 宏 → 运行时 setApplicationVersion
# → 设置页版本标签（原 tr("0.0.1") 硬编码与 CMake 0.1.0 脱节，另案修并入此）
target_compile_definitions(${TGT} PRIVATE LITE_VERSION="${PROJECT_VERSION}")

### qss
qt_add_resources(${TGT} "qss" PREFIX "/" FILES ${qss_rel})

### icon
qt_add_resources(${TGT} "res" PREFIX "/" FILES ${res})

### i18n（第八轮：中文源文本 + Qt Linguist 链路，重启生效）
# qt_add_translations 内部（Qt6LinguistToolsMacros.cmake 实证）：lrelease 把 .ts 编为
# <bin_dir>/lite-harness_en_US.qm（ALL 目标，随主构建生成），并经
# qt_add_resources(PREFIX "/i18n" BASE <bin_dir>) 内嵌 → 运行时路径
# :/i18n/lite-harness_en_US.qm（I18n.cpp 写死此路径，两侧同源核实）。
# .ts 由手动运行 lupdate 维护，**勿用生成的 lite-harness_lupdate 目标**（实证：
# qt_add_translations 无 SOURCE_TARGETS 时延迟收集全部源目标，会把 FluentUI 子工程
# 源码一并扫入 —— 曾灌入 1776 条外部串）。手跑命令须显式限定 src 目录。
# lite-harness_zh_CN.ts 为中文源语言的镜像目录（译文与源文同文）：本身无翻译功能
# （源即中文，运行时任何缺失都回退源文），价值在于 Linguist 审计统一清单 + 与
# en_US 目录结构对称。lupdate 手跑命令须带 -source-language zh_CN -target-language zh_CN。
qt_add_translations(${TGT} TS_FILES i18n/lite-harness_en_US.ts;i18n/lite-harness_zh_CN.ts NO_GENERATE_PLURALS_TS_FILE)
# qtbase 官方中文 qm 已按裁决摘除（免 136KB 二进制入库）：Qt 标准对话框
# （QFileDialog/QMessageBox 等）按钮回退英文，属已知取舍。
# FluentUI 的 Controls.zh-CN.qm 由其静态库自带资源提供（同 /i18n/ 路径），中文不受影响。

target_link_libraries(${TGT} PRIVATE 
    FluentUI::Controls
    FluentUI::Utils
    Qt6::Network
)

# ---- CPack packaging (the single deployment/packaging path) ----
# The old in-place 'deploy' windeployqt custom target was removed as
# redundant; this install+CPack chain is now the only way to produce a
# distributable output. Pipeline: cpack -B build stages into an internal tree
# by running the generated cmake_install.cmake (defaults to Release for
# multi-config VS), then archives it. All app data (QSS/i18n/icon) is
# resource-embedded and every dependency (FluentUI & friends) is a static
# lib, so the package is just lite-harness.exe + Qt runtime + VC runtime,
# produced by the official qt_generate_deploy_app_script (windeployqt under
# the hood, which keeps the VC compiler runtime on by default).
# Pattern mirrors 3rdparty/FluentUI gallery packaging already vendored here.
option(LITE_PACKAGE "Generate CPack ZIP packaging for lite-harness" ON)
if (LITE_PACKAGE)
	# bin/ layout matches QtDeploySupport defaults (windeployqt deploys the Qt
	# DLLs and qt.conf next to the exe under bin/, plugins/translations at the
	# stage root, resolved from qt.conf at runtime).
	install(TARGETS ${TGT} RUNTIME DESTINATION bin)

	# Deploys Qt DLLs/plugins next to the staged exe at install (staging) time.
	qt_generate_deploy_app_script(
		TARGET ${TGT}
		OUTPUT_SCRIPT lite_harness_deploy_script
		NO_UNSUPPORTED_PLATFORM_ERROR)
	install(SCRIPT ${lite_harness_deploy_script})

	# The FluentUI sub-project registers its own install rules in the same tree
	# (Gallery exe + a duplicated Qt runtime under bin/, third-party
	# include/lib/share leftovers from cmark/ads/qmsetup). CMake includes child
	# cmake_install.cmake files before this directory's own rules, so this
	# root-level remove-code reliably runs last and strips them from the stage.
	install(CODE [[
		foreach(_junk include lib share res stylesheet code config i18n)
			file(REMOVE_RECURSE "${CMAKE_INSTALL_PREFIX}/${_junk}")
		endforeach()
		file(GLOB _stage_exes "${CMAKE_INSTALL_PREFIX}/bin/*.exe")
		foreach(_exe IN LISTS _stage_exes)
			if(NOT _exe MATCHES "lite-harness\\.exe$")
				file(REMOVE "${_exe}")
			endif()
		endforeach()
	]])

	set(CPACK_PACKAGE_NAME "${PROJECT_NAME}")
	set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
	set(CPACK_PACKAGE_FILE_NAME "${PROJECT_NAME}-${PROJECT_VERSION}-win64")
	set(CPACK_GENERATOR "ZIP")
	set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
	include(CPack)
endif()
