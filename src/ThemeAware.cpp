#include "ThemeAware.h"

#include <FluStyleSheetUtils.h>
#include <FluThemeUtils.h>

#include <QStyle>
#include <QWidget>

void ThemeAware::bind(const QString &qssFileName, QWidget *widget,
                      const std::function<void()> &extraRefresh)
{
    if (!widget)
        return;

    auto apply = [widget, qssFileName, extraRefresh](FluTheme theme) {
        FluStyleSheetUtils::setQssByFileName(qssFileName, widget, theme);
        // 组件特有行为放在 QSS 重载之后：依赖新样式级联的子级刷新
        // （如 ChatSessionPage 对气泡按 #msgBrowser[role] 重 polish）必须
        // 等页面级 QSS 就位后再做，否则读到的仍是旧主题级联。
        if (extraRefresh)
            extraRefresh();
        // 兜底重 polish 本体：extraRefresh 可能在 QSS 之后改动态属性
        // （如 CollapsibleBlock::refreshIcons 写 collapsed），而 Qt 不会因
        // setProperty 自动重解析属性选择器；且 setStyleSheet 收到与当前
        // 相同的样式串时会被 Qt 短路跳过重建。unpolish/polish 是属性选择器
        // 生效的确定路径，代价可忽略。
        widget->style()->unpolish(widget);
        widget->style()->polish(widget);
    };

    // 首次应用与主题切换走同一条序列，保证「首载即与联动后状态一致」
    apply(FluThemeUtils::getUtils()->getTheme());

    QObject::connect(FluThemeUtils::getUtils(), &FluThemeUtils::themeChanged,
                     widget, [apply](FluTheme theme) { apply(theme); });
}
