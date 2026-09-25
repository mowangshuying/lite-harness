#pragma once

#include <QString>
#include <QWidget>

class QLabel;

/// 只读工作目录展示条：「工作目录  <中间省略的全路径>」
/// （caption QLabel#workDirCaption + 路径 QLabel#workDirPath）。
///
/// 自 NewChatPage / ChatSessionPage 两份同构代码提取：12px 次要灰字（字号在代码里
/// 设定，与 elide 的 QFontMetrics 量纲一致；配色交给宿主页面 QSS 的 QLabel#workDir*
/// 后代选择器——选择器无子代组合符，中间隔本容器一层仍命中），路径 ToolTip 恒为全量、
/// 可见文本随 label 宽度中间省略（eventFilter 捕获 Resize 重算）。
///
/// 浏览按钮可选：enableBrowse() 追加 QToolButton#workDirBrowse 并发 browseRequested()
/// （NewChatPage 用；ChatSessionPage 只读展示不带入口）。目录选取与持久化策略归页面，
/// 本组件只负责展示与交互透传。
class WorkDirPathBar : public QWidget
{
    Q_OBJECT

public:
    explicit WorkDirPathBar(QWidget *parent = nullptr);

    /// 设置展示路径（组件对目录本身无读写语义，页面是数据源）
    void setPath(const QString &path);
    QString path() const { return m_path; }

    /// 追加「浏览」按钮（幂等：重复调用直接忽略），点击经 browseRequested() 透传
    void enableBrowse();

signals:
    /// 浏览按钮被点击（页面连接后弹 QFileDialog 并回调 setPath()）
    void browseRequested();

protected:
    // QLabel Resize 事件：栏宽被页面 resizeEvent 钳制时 label 被动变宽窄，需重算中间省略
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void refreshDisplay();

    QLabel *m_caption = nullptr;
    QLabel *m_pathLabel = nullptr;
    QWidget *m_browseButton = nullptr; // enableBrowse 建的按钮（非空即已启用，作重复调用守卫）
    QString m_path; // 全路径；可见文本是它的省略快照
};
