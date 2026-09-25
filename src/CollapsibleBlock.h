#pragma once

#include <FluWidget.h>
#include <QTextBrowser>

class QLabel;
class QHBoxLayout;
class QPropertyAnimation;
class QScrollArea;
class QTimer;
class QEvent;
class QResizeEvent;

// 可折叠块基类：ThinkingBlock / ToolBlock / TodoCard 同族折叠逻辑的下沉。
// 结构：不透明头部（手动几何、点击切换）+ 内容区，内容以 stackUnder 叠在
// 头部之下，展开时从头部背后滑出（头部底色负责遮挡）。内容区有两种形态：
// 文本类走 QTextBrowser（initContent，文档测量），列表类走 QScrollArea
// （initScrollContent，测量策略由子类覆写 measureContent 自备）；
// 滑动几何 / 可见性 / 动画只操作 m_contentArea 通用指针，两种形态等价共享。
// 动画机制与 FluExpander 同源：QPropertyAnimation 驱动 "contentHeight" 属性
// （300ms OutCubic），setContentHeight 内同步向上遍历父链逐帧 resize
// （直到 window 或滚动区 viewport 为止）；measureContent 将内容区尺寸固定为
// min(自然高度, 上限)，动画期间仅平移不重排。
// 子类差异经虚钩子保留：refreshIcons（主题图标）、liveText（进行态文案）、
// expandedHeightCap（限高策略）、toggleExpanded（是否记忆用户意愿）、
// onGeometryApplied（块宽变化后的副效应）、measureContent/scheduleMeasure
// （内容结构与更新节奏不同时覆写）。
// 主题装配为子类构造尾部的显式 initTheme() 调用（委托 ThemeAware::bind 完成首刷+订阅）：
// 基类构造期绝不调用虚函数——彼时派生成员尚未赋值，首次刷新会访问空指针。
class CollapsibleBlock : public FluWidget
{
    Q_OBJECT
    Q_PROPERTY(int contentHeight READ contentHeight WRITE setContentHeight)

public:
    ~CollapsibleBlock() override;

    void setExpanded(bool expanded);
    bool isExpanded() const { return m_expanded; }

    int contentHeight() const { return m_contentHeight; }
    void setContentHeight(int h);

signals:
    void expandedChanged(bool expanded);
    // 内容区高度随动画进度变化，供外部（气泡/会话页）跟随刷新布局
    void sizeChanged();

protected:
    explicit CollapsibleBlock(QWidget *parent = nullptr);

    // ---- 构造装配管线（子类构造函数按序调用）----

    // 头部容器：固定高 kHeaderHeight、手型光标、不透明底色由 QSS 决定；
    // 返回其 QHBoxLayout（spacing 8），子类自行 addWidget 各标签
    QHBoxLayout *initHeader(const QString &objectName,
                            int left, int top, int right, int bottom);
    // 16x16 居中字形标签（图标/箭头），父挂头部；具体内容与着色由子类决定
    QLabel *createGlyphLabel(const QString &objectName);
    // 标题标签（宽度自适应，文本由子类设置/轮播）
    QLabel *createTitleLabel(const QString &objectName);
    // 内容区（文本形态）：NoFrame / 外链可开 / 横条恒关 / 竖条按需 / 按控件宽折行，
    // stackUnder 到头部之下；同时安装头部点击过滤与 contentsChanged 延迟测量
    void initContent(const QString &objectName);
    // 内容区（列表形态，TodoCard）：QScrollArea 承载子类自建的行容器，
    // NoFrame / 横条恒关 / 竖条按需，与 initContent 共享叠放与点击过滤装配；
    // 返回指针供子类 setWidget 组装列表，自然高度测量需子类覆写 measureContent
    QScrollArea *initScrollContent(const QString &objectName);
    // 主题装配：委托 ThemeAware::bind —— 立即加载 QSS 并首刷 refreshIcons，
    // 再订阅 themeChanged。必须在子类构造函数尾部调用（派生成员已全部就绪）
    void initTheme(const QString &qssFileName);
    // 默认折叠：内容直接隐藏，避免末行文字透过头部半透明 border 渗出
    void initCollapsed();

    // ---- 子类钩子 ----
    virtual void refreshIcons() = 0;             // 箭头方向 / 专属图标着色
    // live 轮播标题文案：仅参与圆点轮播的子类需要覆写；
    // 无进行态的子类（TodoCard）落到默认空串实现（轮播定时器仅在 startLiveTimer 后取文案）
    virtual QString liveText() const;
    // 展开高度上限：默认 m_maxExpandedHeight（ThinkingBlock 进行态压缩为单行）
    virtual int expandedHeightCap() const;
    // 头部点击路径（程序化切换走 setExpanded 不经过此）：
    // ThinkingBlock 覆写以记住用户意愿
    virtual void toggleExpanded();
    // resizeEvent 中头部/内容几何随块宽定位后的钩子（ToolBlock 重排省略号）
    virtual void onGeometryApplied() {}
    // 内容自然高度测量并应用到当前展开态。基类实现面向 QTextBrowser 文档布局，
    // 就地更新按"跳变"应用（流式每 token 触发 contentsChanged，动画会造成风暴）；
    // 内容形态或更新节奏不同的子类覆写（TodoCard：行数公式 + 平滑跟高动画）
    virtual void measureContent();
    // 延迟一帧测量（等布局稳定后宽度确定）。基类实现守卫"动画中测量覆盖终点回弹"；
    // 测量即重定向动画的子类（TodoCard）覆写去掉守卫——动画中途来新数据须即时重定向
    virtual void scheduleMeasure();

    // ---- 折叠动画公共骨架 ----
    // 懒建 contentHeight 属性动画（300ms OutCubic，finished 复位 m_animating）
    QPropertyAnimation *ensureHeightAnimation();
    // 从当前可见高度平滑跟到 target（置 m_animating 并重启懒建好的动画）
    void startHeightAnimation(int target);

    // ---- live 圆点轮播公共骨架（400ms 一个相位，0..3 循环）----
    void startLiveTimer();  // 懒建定时器并启动
    void stopLiveTimer();   // 复位 m_live 并停止（标题文案由子类自行收尾）

    // ---- 共享部件与状态 ----
    QWidget *m_header = nullptr;
    QLabel *m_titleLabel = nullptr;
    QLabel *m_arrowLabel = nullptr;
    QTextBrowser *m_content = nullptr;  // 文本形态内容（Thinking/Tool），尺寸固定为
                                        // min(自然高度, 上限)，动画期间仅靠 move 滑出；
                                        // 列表形态子类不持有
    QWidget *m_contentArea = nullptr;   // 内容区通用指针（指向 m_content 或 QScrollArea），
                                        // 滑动几何 / 可见性 / 遮挡动画只操作此指针

    int m_maxExpandedHeight = 0;    // 常规展开限高（子类构造期赋各自常量）
    bool m_expanded = false;
    bool m_animating = false;       // 动画进行中：禁止 resizeEvent 重新测量
    int m_contentHeight = 0;        // 当前内容可见高度（0=完全折叠），动画驱动属性
    int m_fullContentHeight = 0;    // 展开时完整高度（测量所得，随内容流式增长更新）
    QPropertyAnimation *m_anim = nullptr;

    bool m_live = false;            // 流式进行态
    int m_liveDots = 0;             // 圆点轮播相位 0..3
    QTimer *m_liveTimer = nullptr;  // 懒建 400ms 轮播定时器

    static constexpr int kHeaderHeight = 32;   // 头部固定高（几何公式与 setContentHeight 共用）
    static constexpr int kVerticalChrome = 8;  // 内容区 QSS 上下 padding 各 4px（三主题一致）

private:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void startExpandAnimation();
    int scrollbarExtentWidth() const;
    // 内容区通用挂接：stackUnder 头部之下 + 块最小高度 + 头部点击过滤（两种形态共用）
    void attachContentArea(QWidget *area);
};
