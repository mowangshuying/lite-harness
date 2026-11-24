#pragma once

// 模型管理：服务商选择/新增/删除、服务地址/API Key、自定义模型添加/拉取/移除与每模型偏好。

#include "ModelPrefs.h"
#include "ProviderSpec.h"

#include <QDialog>
#include <QHash>

class QFrame;
class QLabel;
class QVBoxLayout;
class FluComboBox;
class FluLineEdit;
class FluPushButton;
class FluStyleButton;
class FluToggleSwitch;
class FluVScrollView;
class ModelServiceTarget;

class ModelManageDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ModelManageDialog(ModelServiceTarget *target, QWidget *parent);

protected:
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    void rebuildRows();
    QWidget *buildRow(const QString &model);
    void refreshProviderCombo();
    void promptAddProvider();
    void promptRemoveProvider();
    void updateServiceValues();
    void promptEditUrl();
    void promptEditToken();
    void addModel();
    void fetchFromServer();
    void removeCustom(const QString &model);
    void save();

    ModelServiceTarget *m_target = nullptr;
    QWidget *m_parentWidget = nullptr;
    QWidget *m_windowMask = nullptr;
    QFrame *m_card = nullptr;

    FluPushButton *m_fetchButton = nullptr;
    FluPushButton *m_addButton = nullptr;

    FluComboBox *m_providerCombo = nullptr;
    FluPushButton *m_providerAddButton = nullptr;
    FluPushButton *m_providerRemoveButton = nullptr;
    QLabel *m_urlValue = nullptr;
    QLabel *m_tokenValue = nullptr;
    bool m_repopulatingProvider = false;

    FluVScrollView *m_scroll = nullptr;
    QWidget *m_modelPage = nullptr;
    QVBoxLayout *m_modelLayout = nullptr;

    QLabel *m_footerInfo = nullptr;
    FluPushButton *m_cancelButton = nullptr;
    FluStyleButton *m_saveButton = nullptr;

    QHash<QString, ModelPrefs::Prefs> m_pending; // 行控件直写，保存才落盘
    QStringList m_custom;                      // 自定义窗口清单快照
    QStringList m_originalCustom;              // 保存时识别已移除项
    bool m_fetching = false;
};