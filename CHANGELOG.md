# Changelog

## v2.2

- 全局精简源码注释（保留约束与坑，去掉冗长流水账）
- 简化中文 README（功能/接线/构建/配网入口）
- `UI_COLOR_*` 单点定义：声明在 `ui.h`，定义在 `ui.c`（不再放 LCD）
- 修正分区表与 `sdkconfig.defaults` 容量注释（ota_0/ota_1 各 2MB）

## v2.11

- 全库审计修复等历史版本（详见 git log）
