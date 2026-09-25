#pragma once

#include <QString>

#include <functional>

class QWidget;

/// 全仓「QSS 主题化」同构样板的收口点（此前约 12 处各自重复：首载 QSS +
/// connect themeChanged + 回调里重载，个别还漏刷导致行为不一致）。
///
/// bind() 承担公共部分：
///   首次应用   —— 按当前主题加载 QSS（等价原构造尾部 setQssByFileName 一行）；
///   themeChanged —— 重载 QSS → 执行附加刷新回调 → 对 widget 本体 unpolish/polish。
/// 连接以 widget 为 context 对象：widget 销毁即自动断开，闭包不会悬挂
/// （信号源 FluThemeUtils 为进程级单例，生命周期长于任何控件）。
///
/// 各组件特有行为（SVG 图标重着色、子级气泡重 polish 等）不属于样板，
/// 由调用方经 extraRefresh 传入，helper 只吸收「QSS 重载 + polish」这份公共样板。
class ThemeAware
{
public:
    ThemeAware() = delete;

    /// 绑定 QSS 文件与主题联动。须在 widget 构造尾部调用——此时派生成员
    /// 已全部就绪，extraRefresh 里的虚派发/成员访问安全（规避基类构造期
    /// 调虚函数的时序债，见 BasePage 注释）。QSS 文件名解析规则同
    /// FluStyleSheetUtils::setQssByFileName（stylesheet/<theme>/<文件名>.qss）。
    static void bind(const QString &qssFileName, QWidget *widget,
                     const std::function<void()> &extraRefresh = {});
};
