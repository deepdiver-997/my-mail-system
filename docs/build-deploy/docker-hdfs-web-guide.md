# Docker + WebHDFS 启动文档

本文档针对当前仓库的 Docker 运行方式，给出从配置到成功启动的最短路径。

## 1. 前置条件

- 已安装并启动 Docker Desktop。
- 宿主机 MySQL 已启动，且容器可通过 host.docker.internal:3306 访问。
- 在仓库根目录执行命令。

## 2. 目录与挂载约定

应用通过 docker/compose.hdfs-web.yml 挂载以下目录：

- deploy/config
- deploy/certs
- deploy/logs
- deploy/mail
- deploy/attachments

确认目录存在：

```bash
mkdir -p deploy/config deploy/certs deploy/logs deploy/mail deploy/attachments
```

## 3. MySQL 配置

编辑 config/db_config.json（你已完成）。关键项：

- achieve: mysql
- host: host.docker.internal
- user/password: 你的 MySQL 账号密码
- database: mail
- initialize_script: sql/create_tables.sql

说明：create_tables.sql 第一条语句包含 CREATE DATABASE IF NOT EXISTS mail。
如果该账号无建库权限，请先手工创建数据库并授权。

## 4. 证书准备（必须）

当前配置启用了 SMTPS，必须提供以下文件到 deploy/certs：

- server.crt
- server.key
- dkim_private.pem

可用仓库脚本生成：

```bash
./scripts/generate_server_cert.sh -d mail.hgmail.xin -o ./deploy/certs -f
./scripts/generate_dkim_keys.sh -d mail.hgmail.xin -s default -o ./deploy/certs -f
cp ./deploy/certs/default.mail.hgmail.xin.private.pem ./deploy/certs/dkim_private.pem
```

## 5. 首次启动前的 HDFS 权限初始化

首次初始化 NameNode 数据卷时，建议执行一次：

```bash
docker compose -f docker/compose.hdfs-web.yml up -d hdfs-namenode hdfs-datanode
docker exec hdfs-namenode hdfs dfs -mkdir -p /user/hdfs
docker exec hdfs-namenode hdfs dfs -chown -R hdfs:hdfs /user/hdfs
```

## 6. 启动服务

一键构建并启动：

```bash
docker compose -f docker/compose.hdfs-web.yml up -d --build
```

查看状态：

```bash
docker compose -f docker/compose.hdfs-web.yml ps
```

查看应用日志：

```bash
docker logs --tail=200 mail-system
```

预期关键日志：

- Database pool initialized successfully
- PersistentQueue initialized
- 未出现 Error initializing server

## 7. 停止与重启

停止：

```bash
docker compose -f docker/compose.hdfs-web.yml down
```

仅重启应用容器（配置变更后常用）：

```bash
docker compose -f docker/compose.hdfs-web.yml up -d --force-recreate --no-deps mail-system
```

清理并重置 HDFS 卷（谨慎，会清空 HDFS 数据）：

```bash
docker compose -f docker/compose.hdfs-web.yml down -v
```

## 8. 常见问题

1. 报错 Certificate file not found

- 原因：deploy/certs 缺少 server.crt 或 server.key。
- 处理：按第 4 节生成并放入正确文件名。

2. 报错 Access denied for user

- 原因：MySQL 用户/密码不匹配或无权限。
- 处理：检查 config/db_config.json 与 MySQL 授权。

3. 报错 Unknown database mail

- 如果随后日志显示 SQL script execution completed 且服务继续启动，可忽略首次提示。
- 若最终仍失败，检查账号是否有 CREATE DATABASE/CREATE TABLE 权限。

4. 报错 webhdfs mkdirs failed 403

- 原因：HDFS 目录权限不足。
- 处理：执行第 5 节初始化命令，确保 /user/hdfs 归属正确。

5. 容器内访问内部服务走了代理

- 症状：访问 hdfs-namenode 失败但宿主机可访问。
- 处理：compose 中已为 mail-system 清空 HTTP_PROXY/HTTPS_PROXY/ALL_PROXY，并设置 NO_PROXY。
