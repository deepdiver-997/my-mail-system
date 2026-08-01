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
└── style/                             # 规范与总结
    ├── PROJECT_STYLE.md               # 项目编码规范
    └── dev-summary.md                 # 开发总结
```

新 bugfix 文档按 `YYYY-MM-DD-简短描述.md` 格式命名。
