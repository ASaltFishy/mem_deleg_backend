# 设计规范入口

本仓库的设计规范位于内核工作树：
`/home/lrc/patent/kernel/kernel_platform/common/DESIGN.md`。

当前共享日志内核 ABI 为 v3，metadata/broker 为 v9，默认每 CPU Arena 为 256 MiB。
具体交接协议、页表同步、物理缓存回收与安全边界以该规范为准，本文不维护第二份设计副本。

内核设计文档当前按以下章节组织：

- §3.1：统一委托后端与分配器适配。
- §3.2：用户态快速交接与双日志协议。
- §3.3：内核归属保护与延迟页表同步。
- §3.4：运行期物理缓存回收。
- 第四、五章：生命周期、安全边界与后续完善。

当前构建与测试方法见 [README.md](README.md) 和 [tests/README.md](tests/README.md)。
[KERNEL_ALIGNMENT.md](KERNEL_ALIGNMENT.md) 是历史对接记录，不代表当前协议版本或验收状态。
文档更新不构成编译、正确性或性能验证。
