#pragma once

#include <QString>

class QWidget;

/// i18n 第八轮（阶段一）：中英文切换基础设施。
/// 架构裁决：源文本统一中文 + Qt Linguist 翻译链路 + 重启生效
/// （FluentUI 控件文案构造期定死，官方 gallery 以 exit code 931 + 自重启处理语言切换）。
/// 语言权威存储 = exe 同目录 settings.ini 配置文件（AppSettings 单源，键 "language"）；
/// FluentUI 的 setLanguage 只写 CWD 相对的 config.ini 且不发任何信号，仅作镜像同步。
namespace I18n {

/// 读取已存语言："zh-CN" / "en-US"，缺省 "zh-CN"（源文本即中文，无 translator 也正确渲染）
QString language();

/// 写入 settings.ini 配置文件并镜像同步 FluConfigUtils::setLanguage（保持 FluentUI 内部读值一致）
void setLanguage(const QString &lang);

/// 装载 translator：先删旧再装新 QTranslator 对象——重装是 Qt 向全 widget 树
/// 广播 QEvent::LanguageChange 的唯一途径（FluentUI 无 languageChanged 信号可用）。
/// 须在 LiteHarness 构造前调用（App.cpp），使全部控件构造期即取目标语言文案。
void applyLanguage(const QString &lang);

/// 请求重启以生效语言设置：先走 parent 所在顶层窗口的 close()（复用既有运行守卫，
/// 守卫取消则不重启、已存设置留待下次正常启动），通过后 startDetached 拉起新进程
/// 并 qApp->exit(931)。返回是否已发起重启。main 端对 931 只透传、不得重复拉起。
bool requestRestart(QWidget *parent);

} // namespace I18n
