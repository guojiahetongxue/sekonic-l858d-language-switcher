# Changelog / 更新记录

## 0.4.0 — 2026-10-16

### 中文

- 将明确支持的固件限定为 **17、18、19**，新增 18；继续检查地区、序列号、当前语言及设备组件提供的修改权限。
- 其他固件停止操作，提示使用世光官方 DTS 检查是否可升级到 19。工具不执行固件升级或降级。
- 保留中文、英文、日文切换及独立的中英应用界面；完善双语提示和离线自检。
- 说明同一台日本地区设备的实际验证范围，包括 18 的部分语言测试及尚未确认的重启结果；其他地区仍仅有协议分析与离线测试。
- 提供 macOS 13+ 的 Apple 芯片 / Intel 通用应用，以及构建、测试源码和双语文档。

### English

- Set the explicit firmware allowlist to **17, 18 and 19**, adding 18 while retaining region, serial, current-language and device-capability checks.
- Stop on other firmware versions and direct users to official Sekonic DTS to check for an available upgrade to 19. The app performs no firmware upgrade or downgrade.
- Retain Chinese, English and Japanese meter languages with an independent Chinese/English app interface; refine bilingual guidance and offline self-checks.
- Document the actual scope of testing on one Japanese-region unit, including partial firmware 18 coverage and unconfirmed restart results. Other regions still have protocol analysis and offline tests only.
- Provide a universal macOS 13+ app for Apple silicon and Intel, plus build/test sources and bilingual documentation.
