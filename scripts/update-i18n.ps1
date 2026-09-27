# i18n 词条成对手刷脚本（第十一轮 F3c）
#
# 【禁跑 CMake 陷阱目标】不要用 lite-harness_lupdate（qt_add_translations 自动生成的
# target）——实证会把 FluentUI 子工程 gallery 源码扫入、灌入上千条外部串。
# 本脚本 = AGENTS.md「新增/修改 UI 字符串」条目所述两条手动命令的固化：
# 必须带 -ts 成对刷新 zh 镜像与 en 目录，缺 -ts 会原地改源文件语言。
# 跑完后：给 en_US.ts 新条目补英文译文、zh_CN.ts 保持译文=源文，再正常构建（lrelease 内嵌）。
$Lupdate = "C:/Qt/6.9.0/msvc2022_64/bin/lupdate.exe"
& $Lupdate -recursive src -no-obsolete -source-language zh_CN -target-language zh_CN -ts i18n/lite-harness_zh_CN.ts
& $Lupdate -recursive src -no-obsolete -source-language zh_CN -target-language en_US -ts i18n/lite-harness_en_US.ts
