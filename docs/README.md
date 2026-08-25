# ProtoRelay 文档

## 目录结构

```
docs/
├── README.md                          # 本文件
├── architecture/                      # 架构与设计文档
│   ├── ARCHITECTURE.md                # 整体架构
│   ├── architecture-evolution.md      # 架构演进历史
│   ├── framework-refactor.md          # 框架重构记录
│   ├── imap-protocol-flow.md          # IMAP 协议流程
│   ├── imap-server-design.md          # IMAP 服务设计
│   ├── smtp-outbound-client-design.md # SMTP 发件引擎设计
│   ├── sharding-refactor.md           # 分片重构
│   └── vs-postfix.md                  # 与 Postfix 的对比
├── build-deploy/                      # 构建、部署、运维
│   ├── cross-compile-guide.md         # 交叉编译指南
│   ├── docker-hdfs-web-guide.md       # Docker + HDFS Web 指南
│   ├── log-transform-toolchain.md     # 日志变换工具链
│   ├── logging-guide.md               # 日志指南
│   ├── operations.md                  # 运维文档
│   └── quick-log-config.md            # 快速日志配置
├── bugfixes/                          # 问题修复记录
│   ├── 2026-08-01-smtp-imap-deploy-fixes.md
│   ├── imap-cpu-busyloop-fix.md       # IMAP CPU 忙等修复
│   └── prepared-statement-connection-pool-issue.md  # DB 连接池修复
├── reports/                           # 测试覆盖率报告（量化成果）
│   └── coverage-YYYY-MM-DD.md         # 由 test/scripts/coverage.sh 生成
└── style/                             # 规范与总结
    ├── PROJECT_STYLE.md               # 项目编码规范
    └── dev-summary.md                 # 开发总结
```

新 bugfix 文档按 `YYYY-MM-DD-简短描述.md` 格式命名。

## 测试覆盖率报告

`test/scripts/coverage.sh` 一键测量单测的源码行覆盖率，生成 `docs/reports/coverage-<日期>.md`
（模块汇总 + framework 层逐文件 + 最低覆盖 Top15）与 `build-cov/html/`（HTML 详情）。
CI 的 coverage job 也会产出并上传 HTML artifact。

## 一次性任务文档

**任务/TODO 类文档不要放进 `docs/`**（尤其 `architecture/`，那里只放长期有效的设计）。
放到仓库根目录 `tasks/`（已被 `.gitignore` 排除，不纳入版本控制），任务做完即删。
经验总结如需保留，可在 `tasks/` 里写笔记，或提炼到 `docs/` 的长期文档中。

