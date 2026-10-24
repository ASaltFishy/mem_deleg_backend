# 最终设计入口

本仓库的最终规范为内核工作树：
`/home/lrc/patent/kernel/kernel_platform/common/DESIGN.md`。

本文件不再保留与内核 §2.10/§2.11/§2.12 冲突的历史 v3 设计副本。
2026-10-22 用户确认：Mutex 持有者允许迁核，迁核后的 FREE 等回执；随后确认同核 FREE 不等回执并去掉跨进程自动清零。
实现恢复同核 FREE 日志发布后的立即入池，已迁出 owner CPU 与跨核 FREE 继续等待成功回执。
异常退出采用保守处理、不做自动恢复；恶意发布者认证暂缓。

ABI 和测试范围见 `KERNEL_ALIGNMENT.md`；历史原文已保存在本次改动前的源码备份中。


## 2026-10-22 性能实验更新

最终设计以内核 DESIGN.md §2.12 为准。同核普通 FREE 发布日志后、确认仍在 owner CPU，即可直接发布空闲候选；远端及已经迁核的 FREE 等回执。获取者在 Mutex 后重查 CPU，持锁迁核本身仍允许。
跨进程/跨 session/混合来源的自动整块清零已删除；MD_EAGER_CLEAR 自动策略已删除。显式 calloc、MD_ZERO 和调用方明确要求的清零保留语义。仍保留所有权、代际、日志与页表协议，未关闭内核桥接。
共享 metadata/broker 协议为 v8，默认域 allocator_delegation_v8、目录 md_backend_v8；内核 ABI 为 v3。必须使用匹配的新内核，不能复用当前启动的 v7 backing。普通成功日志由内核直接回收，远端回执仍由用户态确认。

当前协议细节见内核 DESIGN.md §2.17 与 tools/testing/memory_delegation/SHARED_LOG_V3.md。本次构建未部署到手机；新二进制不能在旧 v2 内核上运行。
