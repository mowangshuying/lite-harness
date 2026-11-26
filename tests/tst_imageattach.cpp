// tst_imageattach.cpp —— 图片附件载荷判据直测（src/ImageAttach.h，header-only 零 CMake 改动）
// 被测：魔数嗅探、MIME↔扩展名两向、内容哈希命名、附件名形态守卫、引用 scheme 两向、
//       体量/张数闸门、data URL 拼装
//
// 本套件的意义分两类，都不容「跑通即可」：
//   ① **文件 IO 的前置闸门**（isSafeName / nameFromUrl）。这两个口的入参来自盘上
//      history.json，而它们的输出决定应用去读哪个文件。松一寸就等于让一份手改的历史
//      指定任意路径去读——与 UpdateRules::normalizeEntryPath 的 zip-slip 防线、
//      CompactTranscript 的 .transcripts 目录守卫同一族护栏，负例比正例重要。
//   ② **类型判定的独立性**。mimeFor 只读字节，绝不经过 QImage/图像插件：发布包的
//      plugins/imageformats 在本机与 CI 产物里条目数不同（实测差 5 枚 imageformats），
//      若判据依赖解码器，同一份历史在两台机器上会给出不同 MIME。
//
// 不守什么（诚实边界）：
//   ① 落盘与读盘（QSaveFile / 目录创建）在宿主侧，本头零 IO；
//   ② 缩略图渲染、剪贴板取图、拖拽入口长在 Qt6::Gui/Widgets 上，进不了 ctest；
//   ③ 「端点是否收 data URL」是网络侧事实，须实测取证，不由本头断言。
//
// 零夹具依赖（不读 LITE_TEST_TMPROOT），任何环境全量执行。

#include "TestHarness.h"

#include "ImageAttach.h"
#include "AgentConstants.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <initializer_list>

namespace {

// 按字节值构造载荷：>127 的十六进制转义在 /utf-8 下的窄字面量里是实现定义行为
// （MSVC C4566 一族），故一律走数值，与 ImageAttach::hasMagic 同口径。
QByteArray raw(std::initializer_list<int> values)
{
    QByteArray out;
    for (const int value : values)
        out.append(static_cast<char>(value));
    return out;
}

QByteArray pngBytes()
{
    QByteArray out = raw({0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A});
    out += "IHDR-fake-scanline-payload";
    return out;
}

QByteArray jpegBytes()
{
    QByteArray out = raw({0xFF, 0xD8, 0xFF, 0xE0});
    out += "Exif-fake-payload";
    return out;
}

QByteArray webpBytes()
{
    QByteArray out = raw({0x52, 0x49, 0x46, 0x46});
    out += "1234WEBPVP8 fake";
    return out;
}

QString validName()
{
    return ImageAttach::nameFor(pngBytes(), QStringLiteral("image/png"));
}

// ① MIME 嗅探：五种支持形态各自命中
void testSniffPositive()
{
    TestHarness::check(
        ImageAttach::mimeFor(pngBytes()) == QLatin1String("image/png"),
        "PNG 八字节魔数命中 image/png");
    TestHarness::check(
        ImageAttach::mimeFor(jpegBytes()) == QLatin1String("image/jpeg"),
        "JPEG FFD8FF 命中 image/jpeg");
    TestHarness::check(
        ImageAttach::mimeFor(QByteArray("GIF87a....")) == QLatin1String("image/gif"),
        "GIF87a 命中 image/gif");
    TestHarness::check(
        ImageAttach::mimeFor(QByteArray("GIF89a....")) == QLatin1String("image/gif"),
        "GIF89a 命中 image/gif");
    TestHarness::check(
        ImageAttach::mimeFor(QByteArray("BMbits")) == QLatin1String("image/bmp"),
        "BM 命中 image/bmp");
    TestHarness::check(
        ImageAttach::mimeFor(webpBytes()) == QLatin1String("image/webp"),
        "RIFF....WEBP 命中 image/webp");
}

// ① 的负例：判据不得靠 RIFF 前缀或短前缀蒙
void testSniffNegative()
{
    TestHarness::check(ImageAttach::mimeFor(QByteArray()).isEmpty(),
                       "空字节认不出（不猜类型）");
    TestHarness::check(ImageAttach::mimeFor(QByteArray("hello world")).isEmpty(),
                       "纯文本不是图");
    QByteArray riffOnly = raw({0x52, 0x49, 0x46, 0x46});
    riffOnly += "1234AVI ";
    TestHarness::check(riffOnly.startsWith("RIFF") && ImageAttach::mimeFor(riffOnly).isEmpty(),
                       "RIFF 但非 WEBP（AVI 也是 RIFF 容器）必须落空");
    QByteArray pngTruncated = pngBytes().left(6);
    TestHarness::check(ImageAttach::mimeFor(pngTruncated).isEmpty(),
                       "PNG 魔数只到 6 字节即拒（前缀不足不误判）");
    TestHarness::check(ImageAttach::mimeFor(raw({0xFF, 0xD8})).isEmpty(),
                       "JPEG 缺第三字节 FFD8 即拒");
    TestHarness::check(ImageAttach::mimeFor(QByteArray("GIF8")).isEmpty(),
                       "GIF 主版本段不完整即拒");
    QByteArray nearPng = pngBytes();
    nearPng[1] = 'X';
    TestHarness::check(ImageAttach::mimeFor(nearPng).isEmpty(),
                       "PNG 魔数中间一位不同即拒（逐字节比对生效）");
}

// ② MIME ↔ 扩展名两向：每个支持项往返回原值，未知项两向都落空
void testExtensionRoundTrip()
{
    const QStringList mimes = {QStringLiteral("image/png"), QStringLiteral("image/jpeg"),
                               QStringLiteral("image/gif"), QStringLiteral("image/bmp"),
                               QStringLiteral("image/webp")};
    int checked = 0;
    for (const QString &mime : mimes) {
        const QString ext = ImageAttach::extensionFor(mime);
        TestHarness::check(!ext.isEmpty() && ImageAttach::mimeForExtension(ext) == mime,
                           "支持的 MIME 扩展名两向往返等值");
        ++checked;
    }
    TestHarness::check(checked == 5, "五种支持类型全部参与往返（名单没漏项）");
    TestHarness::check(ImageAttach::extensionFor(QStringLiteral("image/tiff")).isEmpty(),
                       "不支持的 image/tiff 无扩展名");
    TestHarness::check(ImageAttach::mimeForExtension(QStringLiteral("svg")).isEmpty(),
                       "不支持的 svg 无 MIME（SVG 是文本，喂给视觉模型无意义且可携脚本）");
    TestHarness::check(ImageAttach::mimeForExtension(QString()).isEmpty()
                           && ImageAttach::extensionFor(QString()).isEmpty(),
                       "空串两向都落空，不回落成某个默认类型");
}

// ③ 命名：内容哈希、确定性、形态
void testNaming()
{
    const QString name = validName();
    TestHarness::check(!name.isEmpty() && name.size() == 20 && name.at(16) == QLatin1Char('.'),
                       "附件名形态 = 16 位哈希 + 点 + 扩展名");
    TestHarness::check(name.endsWith(QStringLiteral(".png")), "扩展名由 MIME 决定");
    TestHarness::check(name == validName(), "同图同字节两次命名逐字相同（无随机、无序号）");
    TestHarness::check(ImageAttach::nameFor(jpegBytes(), QStringLiteral("image/jpeg"))
                           != name,
                       "不同字节给出不同名（内容哈希真的参与命名）");
    TestHarness::check(ImageAttach::nameFor(pngBytes(), QStringLiteral("image/jpeg"))
                           .endsWith(QStringLiteral(".jpg")),
                       "同一字节配不同 MIME 只换扩展名（哈希段不变）");
    TestHarness::check(ImageAttach::nameFor(QByteArray(), QStringLiteral("image/png")).isEmpty(),
                       "空载荷无命名（不产出仅扩展名的半截名）");
    TestHarness::check(ImageAttach::nameFor(QByteArray("x"), QStringLiteral("image/tiff")).isEmpty(),
                       "不支持类型无命名");
    const QString lower = name.left(16);
    bool allLowerHex = true;
    for (const QChar &ch : lower)
        allLowerHex = allLowerHex
            && ((ch >= QLatin1Char('0') && ch <= QLatin1Char('9'))
                || (ch >= QLatin1Char('a') && ch <= QLatin1Char('f')));
    TestHarness::check(allLowerHex, "哈希段全小写十六进制（大小写敏感的盘上也稳）");
}

// ④ 形态守卫：越界写法一律拒（本套件最要紧的一组负例）
void testNameGuard()
{
    TestHarness::check(ImageAttach::isSafeName(validName()), "自身产出的名必自认合法");

    const QStringList evil = {
        QStringLiteral(""),
        QStringLiteral(".."),
        QStringLiteral("../../etc/passwd"),
        validName() + QStringLiteral("/../../x"),
        QStringLiteral("a/b.png"),
        QStringLiteral("a\\b.png"),
        QStringLiteral("C:abs.png"),
        QStringLiteral(".png"),
        QStringLiteral("short.png"),
        QStringLiteral("0123456789abcdef.PNG"),   // 大写哈希段
        QStringLiteral("0123456789abcdef.EXE"),   // 非图扩展名
        QStringLiteral("0123456789abcde.png"),    // 哈希段 15 位
        QStringLiteral("0123456789abcdef1.png"),  // 哈希段 17 位
        QStringLiteral("0123456789abcdeg.png"),   // 非十六进制字符
        QStringLiteral("0123456789abcdef.png.png"),
        QStringLiteral("0123456789abcdef."),
        QStringLiteral(" 0123456789abcdef.png"),
        QStringLiteral("lh-attach://0123456789abcdef.png"),
    };
    for (const QString &name : evil) {
        TestHarness::check(!ImageAttach::isSafeName(name),
                           "非法附件名一律拒（含分隔符/盘符/越界段/形态偏移）");
        TestHarness::check(ImageAttach::urlFor(name).isEmpty(),
                           "非法名不得产出引用（宁缺勿错）");
    }
}

// ⑤ 引用两向
void testUrlRoundTrip()
{
    const QString name = validName();
    const QString url = ImageAttach::urlFor(name);
    TestHarness::check(url == ImageAttach::attachmentScheme() + name,
                       "引用 = scheme + 裸名，不拼目录（目录是宿主的事）");
    TestHarness::check(ImageAttach::nameFromUrl(url) == name, "引用可原样反查回裸名");
    TestHarness::check(ImageAttach::nameFromUrl(QStringLiteral("https://x/a.png")).isEmpty(),
                       "外部 url 反查落空（不被误当成自家附件去读盘）");
    TestHarness::check(
        ImageAttach::nameFromUrl(QStringLiteral("data:image/png;base64,AAAA")).isEmpty(),
        "data url 反查落空（内联形态走 isInlineUrl 另一条判据）");
    TestHarness::check(ImageAttach::nameFromUrl(ImageAttach::attachmentScheme()).isEmpty(),
                       "scheme 后裸空反查落空");
    TestHarness::check(
        ImageAttach::nameFromUrl(ImageAttach::attachmentScheme()
                                 + QStringLiteral("../../secret"))
            .isEmpty(),
        "scheme 后越界段反查落空（读盘入口的最后一道）");
    TestHarness::check(ImageAttach::nameFromUrl(QStringLiteral("lh-attach:/x.png")).isEmpty(),
                       "单斜杠写法不被认成本仓 scheme");
}

// ⑥ 内联判定
void testInline()
{
    TestHarness::check(ImageAttach::isInlineUrl(QStringLiteral("data:image/png;base64,AA")),
                       "data: 前缀判为内联");
    TestHarness::check(!ImageAttach::isInlineUrl(QStringLiteral("https://x/a.png")),
                       "https 不是内联");
    TestHarness::check(!ImageAttach::isInlineUrl(ImageAttach::urlFor(validName())),
                       "本仓引用不是内联（两条判据互斥）");
    TestHarness::check(!ImageAttach::isInlineUrl(QString()), "空串不是内联");
}

// ⑦ 闸门边界（含 0 与负数的拒收方向）
void testGates()
{
    TestHarness::check(ImageAttach::withinByteCap(1), "1 字节过闸");
    TestHarness::check(ImageAttach::withinByteCap(AgentConst::kImageMaxBytes),
                       "恰等于上限过闸（闭区间）");
    TestHarness::check(!ImageAttach::withinByteCap(AgentConst::kImageMaxBytes + 1),
                       "超上限 1 字节即拒");
    TestHarness::check(!ImageAttach::withinByteCap(0), "0 字节拒（空载荷不是图）");
    TestHarness::check(!ImageAttach::withinByteCap(-1), "负数拒");

    TestHarness::check(ImageAttach::withinCountCap(0), "0 张合法（无附件本就是常态）");
    TestHarness::check(ImageAttach::withinCountCap(AgentConst::kImageMaxPerMessage - 1),
                       "上限前一档过闸");
    TestHarness::check(!ImageAttach::withinCountCap(AgentConst::kImageMaxPerMessage),
                       "达到上限即拒（调用方据此拒绝再添加，不是加完再裁）");
    TestHarness::check(!ImageAttach::withinCountCap(-1), "负数张数拒");
}

// ⑧ data URL 拼装
void testDataUrl()
{
    const QByteArray payload = pngBytes();
    const QString url =
        ImageAttach::dataUrlFor(QStringLiteral("image/png"), payload);
    TestHarness::check(url.startsWith(QStringLiteral("data:image/png;base64,")),
                       "data URL 前缀逐字形态");
    TestHarness::check(!url.contains(QLatin1Char('\n')) && !url.contains(QLatin1Char('\r')),
                       "base64 段不含换行（折行的 data URL 会被端点判成坏 url）");
    const QString body = url.mid(QStringLiteral("data:image/png;base64,").size());
    TestHarness::check(QByteArray::fromBase64(body.toLatin1()) == payload,
                       "data URL 逐字节还原回原载荷");
    TestHarness::check(ImageAttach::dataUrlFor(QStringLiteral("image/png"), QByteArray())
                           .isEmpty(),
                       "空载荷不产出 data:image/png;base64, 半截形态");
    TestHarness::check(ImageAttach::dataUrlFor(QString(), payload).isEmpty(),
                       "空 MIME 不产出无类型 data URL");
    TestHarness::check(
        ImageAttach::dataUrlFor(QStringLiteral("image/tiff"), payload)
            .startsWith(QStringLiteral("data:image/tiff;base64,")),
        "本函数只拼装不裁决类型（支持集判定归 mimeFor/extensionFor，两层不互相越权）");
}

} // namespace

int tst_imageattach()
{
    testSniffPositive();
    testSniffNegative();
    testExtensionRoundTrip();
    testNaming();
    testNameGuard();
    testUrlRoundTrip();
    testInline();
    testGates();
    testDataUrl();
    return 0;
}
