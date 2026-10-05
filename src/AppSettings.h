#pragma once

// 设置存储单源头：全仓用户配置一律落 exe 同目录 settings.ini（用户裁决：弃用注册表，
// 禁再引入默认构造 QSettings——org/app 注册表路径已成历史）。
// 键清单：defaultWorkDir / sidebarVisible / language / contextCharLimit / maxToolIterations
// / apiBaseUrl / apiToken / modelOptions / defaultModel / maxRetries。
// apiToken 明文存 ini 属用户裁决（设置页配置取代环境变量，本机 exe 同目录文件，属主可见可改）。
// 手工篡改/非法值由各消费点自带校验回退默认（既有语义，本头不做聚合校验）。
// QSettings 隐式共享，可值返回；局部对象析构即 sync，保持各处原有
// 「局部对象写后即落盘」语义不变。
#include <QCoreApplication>
#include <QSettings>
#include <QString>

namespace AppSettings {

inline QSettings ini()
{
    return QSettings(QCoreApplication::applicationDirPath() + QStringLiteral("/settings.ini"),
                     QSettings::IniFormat);
}

} // namespace AppSettings
