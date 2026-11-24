#pragma once

// 程序图标单源：res/LiteHarness.svg（矢量）。Qt 侧一律经本头按实际像素/DPR 现渲染，
// 任意尺寸锐利；exe/任务栏的 .ico 由同一 SVG 离线导出多档（见 res/LiteHarness.ico）。
// 仅主目标使用（链 Qt6::Svg）；测试目标不 include 本头。

#include <QGuiApplication>
#include <QIcon>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QScreen>
#include <QSvgRenderer>

namespace AppIcon {

inline QPixmap pixmap(int logicalSize)
{
    const QScreen *screen = QGuiApplication::primaryScreen();
    const qreal dpr = screen ? screen->devicePixelRatio() : 1.0;
    const int px = qMax(1, qRound(logicalSize * dpr));
    QImage image(px, px, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QSvgRenderer renderer(QStringLiteral(":/res/LiteHarness.svg"));
    QPainter painter(&image);
    renderer.render(&painter, QRectF(0, 0, px, px));
    painter.end();
    QPixmap pm = QPixmap::fromImage(image);
    pm.setDevicePixelRatio(dpr);
    return pm;
}

inline QIcon icon()
{
    QIcon ic;
    for (int size : {16, 24, 32, 48, 64, 128, 256})
        ic.addPixmap(pixmap(size));
    return ic;
}

} // namespace AppIcon