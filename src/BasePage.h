#pragma once

#include <FluWidget.h>

class BasePage : public FluWidget
{
    Q_OBJECT
public:
    BasePage(QWidget* parent = nullptr);

    virtual ~BasePage();

    void onThemeChanged() override;
};