# 聊天会话页面设计方案

## 页面架构

```
┌──────────┬──────────────────────────────────────────┐
│  NavView │  FluStackedLayout                        │
│          │  ┌─────────────────────────────────────┐ │
│ New Chat │  │  NewChatPage (空状态)                │ │
│          │  │  - Logo + 欢迎语 + ChatMsgEdit      │ │
│ Settings │  └─────────────────────────────────────┘ │
│          │  ┌─────────────────────────────────────┐ │
│          │  │  ChatSessionPage (会话中)            │ │
│          │  │  ┌─────────────────────────────────┐│ │
│          │  │  │  FluVScrollView (消息列表)       ││ │
│          │  │  │  ┌─────────────────────────────┐││ │
│          │  │  │  │  用户消息 (右对齐气泡)       │││ │
│          │  │  │  │  AI回复 (左对齐，无气泡)     │││ │
│          │  │  │  │  ...                        │││ │
│          │  │  │  └─────────────────────────────┘││ │
│          │  │  └─────────────────────────────────┘│ │
│          │  │  ┌─────────────────────────────────┐│ │
│          │  │  │  ChatMsgEdit (输入框)            ││ │
│          │  │  └─────────────────────────────────┘│ │
│          │  └─────────────────────────────────────┘ │
└──────────┴──────────────────────────────────────────┘
```

## 设计风格

参照 Ollama 风格：

```
┌─────────────────────────────────────────┐
│                                    用户  │ ← 右侧气泡，主题色背景
│                                         │
│ AI 回复内容直接显示在这里，               │ ← 无气泡、无头像
│ 支持 Markdown，左对齐                     │
│                                         │
│                                    用户  │
│                                         │
│ 另一条回复...                             │
└─────────────────────────────────────────┘
```

## 新增文件

| 文件 | 说明 |
|------|------|
| `ChatSessionPage.cpp/h` | 聊天会话页面 |
| `MessageBubbleWidget.cpp/h` | 消息气泡组件 |
| `stylesheet/*/ChatSessionPage.qss` | 主题样式 (3套) |

## 核心组件

### MessageBubbleWidget

```cpp
class MessageBubbleWidget : public FluWidget {
    enum Role { User, Assistant };
    
    QVBoxLayout* m_layout;
    QTextBrowser* m_content;    // Markdown 渲染
    
public:
    void setContent(const QString& markdown) {
        m_content->setMarkdown(markdown);
    }
};
```

**样式对比：**

| Role | 背景 | 对齐 | 头像 | 边框/圆角 |
|------|------|------|------|-----------|
| User | 主题色 | 右对齐 | 无 | 圆角气泡 |
| Assistant | 透明 | 左对齐 | 无 | 无 |

### ChatSessionPage

```cpp
class ChatSessionPage : public BasePage {
    QVBoxLayout* m_mainLayout;
    FluVScrollView* m_scrollView;  // 消息列表
    ChatMsgEdit* m_inputEdit;      // 底部输入框
    
public:
    void addMessage(Role role, const QString& content);
    void scrollToBottom();
    void clearMessages();
};
```

## Markdown 支持

使用 `QTextBrowser::setMarkdown()` 原生渲染：

- ✅ 标题、列表、粗体、斜体
- ✅ 链接
- ✅ 行内代码
- ✅ 表格 (Qt 6.4+)
- ⚠️ 代码块：灰色背景，无语法高亮

## LiteHarness 修改

1. 移除 History Sessions 导航项
2. 注册 ChatSessionPage 到 `m_sLayout`
3. 连接 NewChatPage 发送信号 → 切换到 ChatSessionPage

## 交互流程

1. 用户在 NewChatPage 输入消息，点击发送
2. 切换到 ChatSessionPage
3. 调用 `addMessage(User, content)` 添加用户消息
4. 调用 `addMessage(Assistant, response)` 添加回复
5. 后续对话在此页面继续

## 优势

- **简洁**：仅 2 个新组件，无 Model/Delegate 复杂性
- **灵活**：每个气泡是独立 Widget，易于扩展
- **原生**：直接使用 FluentUI 的 FluVScrollView，风格统一
