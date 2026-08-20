# AGENTS.md — lite-harness

## 构建

- **工具链:** Qt 6.9.0 + MSVC 2022 + CMake 3.20+
- **Prefix:** `CMAKE_PREFIX_PATH=C:\Qt\6.9.0\msvc2022_64`
- **构建目录:** `build/` (VS 解决方案位于 `build/lite-harness.sln`)
- **输出路径:** `build/bin/lite-harness.exe`
- **C++17**, 仅 Windows (MSVC_IDE)
- **无测试、无 CI、无 lint 配置** — 通过构建和运行验证

## 架构

单可执行文件 (`lite-harness`)，使用 **FluentUI** (git 子模块位于 `3rdparty/FluentUI`) 和 **FramelessHelper** (内置于 FluentUI)。

```
src/
  App.cpp              — QApplication 入口
  LiteHarness.cpp/h    — 主窗口 (FluFrameLessWidget)，导航 + 堆叠布局
  BasePage.cpp/h       — 所有页面的基类
  NewChatPage.cpp/h    — 聊天输入页
  SettingsPage.cpp/h   — 设置页
  ChatMsgEdit.cpp/h    — 自定义文本编辑器，支持主题 QSS
  SendMsgButton.cpp/h  — 圆形 SVG 图标按钮，动态主题着色
```

## 主题 / QSS 系统

- 三套主题: `light`, `dark`, `atomOneDark`
- QSS 文件: `stylesheet/{light,dark,atomOneDark}/<WidgetName>.qss`
- 加载方式: `FluStyleSheetUtils::setQssByFileName("WidgetName.qss", widget, theme)`
- 路径解析: `stylesheetDir + themeName + "/" + filename`，其中 stylesheetDir 为 `../stylesheet/` (调试) 或 `:/stylesheet/` (QRC)
- **新增带 QSS 的组件:** 在 `CMakeLists.txt` 中将 `.cpp/.h` 加入 `src` 列表，将 `.qss` 加入 `qss` 列表，然后在构造函数中调用 `setQssByFileName` 并连接 `FluThemeUtils::themeChanged` 信号
- 主题切换: 监听 `FluThemeUtils::getUtils()->themeChanged` 信号

## 关键约定

- 导航: `FluVNavigationView` + `FluStackedLayout`，使用字符串键
- 页面注册: 创建页面 → `m_sLayout->addWidget("Key", page)` → 导航项使用相同键
- FluentUI 头文件: `<FluUtils.h>`, `<FluThemeUtils.h>`, `<FluStyleSheetUtils.h>` 等 — 位于 `3rdparty/FluentUI/controls` 和 `3rdparty/FluentUI/utils`
- 子模块: 首次构建前执行 `git submodule update --init` 获取 

## Git 提交
采用中文日志
