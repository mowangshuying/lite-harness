#pragma once

#include "CollapsibleBlock.h"

class QLabel;
class QTimer;

// 思考过程折叠块（Ollama 风格）：
//   标题栏（灯泡图标 + "思考了 N 秒" + 折叠箭头）+ 可展开/折叠的思考内容区。
// 折叠/动画/测量骨架已下沉 CollapsibleBlock（与 ToolBlock 逐行等价的部分）；
// 本类仅保留思考专属差异：灯泡图标、耗时文案、live 轮播文案（"思考中"+圆点+已耗时）、
// 进行态单行限高策略与用户手动操作记忆。
class ThinkingBlock : public CollapsibleBlock
{
    Q_OBJECT

public:
    // 思考内容可见区最大高度（px）：ThinkingBlock 展开高度与流式思考气泡共用
    static constexpr int kMaxThinkingHeight = 150;

    explicit ThinkingBlock(QWidget *parent = nullptr);

    void setThinkingContent(const QString &thinkingText);
    void setThinkingDuration(int seconds);

    // Static terminal form for session replay: history.json persists no thinking
    // duration, so the title shows a neutral "done" string instead of a fake
    // seconds sentence. No live timer, block keeps the collapsed default.
    void finishWithoutDuration();

    // ---- 流式进行态（思考生成期间占位展示）----
    // startLive：头部切换为「思考中」（圆点轮播 + 已耗时逐秒跳动），未被打扰时自动展开，
    //   展开可见高度压缩为单行文本，钉底滚动只显示最新一行思考内容；
    // stopLive：恢复终态「思考了 N 秒」，未被打扰时自动折叠；
    //   此后手动展开按完整限高（kMaxThinkingHeight）显示全部内容。
    // appendLiveText：增量纯文本追加，单行视口内滚动并跟随最新内容（钉底）。
    // 用户手动点过头部后，进行/终态切换不再自动改变展开状态（尊重用户操作），
    // 且进行态恢复完整限高视图（视为用户主动要求查看更多）。
    void startLive();
    void stopLive(int seconds);
    void appendLiveText(const QString &delta);
    bool isLive() const { return m_live; }

    // setExpanded / isExpanded / contentHeight / setContentHeight 继承自基类（API 冻结）

protected:
    // ---- CollapsibleBlock 钩子 ----
    void refreshIcons() override;              // 灯泡 + 箭头方向
    QString liveText() const override;         // 「思考中」+ 轮播圆点 + 已耗时
    void onLiveTick() override;                // 400ms 轮播 tick：刷新顶部预览行（当前输出行）
    int expandedHeightCap() const override;    // 终态展开限高（进行态不允许展开，故无压缩分支）
    void toggleExpanded() override;            // 进行态拒绝展开；终态记忆用户意愿
    void measureContent() override;            // 折叠态跳过测量：流式期间省掉文档布局 pass

private:
    QString durationText() const;
    QString durationPhrase(int seconds) const;   // 时长短语单源（"< 1 秒"/"N 秒"/"N 分 M 秒"）
    void updatePreview();                        // 取 m_liveTail 的当前行写入预览标签

    QLabel *m_iconLabel = nullptr;
    QLabel *m_previewLabel = nullptr;   // 进行态顶部预览（当前输出行，ElideRight 单行）
    QString m_liveTail;                 // 进行态思考文本末尾窗口（取当前行用，截断防常驻）
    QString m_previewText;              // 上一次非空预览行（刚换行时不闪空）
    int m_durationSeconds = 0;      // 思考耗时（秒）
};
