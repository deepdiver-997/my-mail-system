# MySQL Prepared Statement 与连接池的兼容性问题分析

## 问题描述

在使用 MySQL prepared statement 时，虽然日志显示参数绑定正确且 `affected_rows: 1`，但数据库中实际保存的数据全是空的。而使用 `escape_string()` + 直接 SQL 执行的方式则能正常工作。

## 排查过程

### 1. 初步假设：缓冲区生命周期问题

**假设**：prepared statement 的参数绑定中，`binds[i].buffer` 指向的字符串缓冲区可能在 `mysql_stmt_execute()` 期间失效。

**验证**：
- 修改代码创建参数副本：`std::vector<std::string> param_copies(params);`
- 使用副本指针：`binds[i].buffer = (void*)param_copies[i].c_str();`
- 测试结果：问题依然存在，证明不是缓冲区生命周期问题

### 2. 深入分析：连接池验证机制的影响

**发现**：
```cpp
// mysql_pool.cpp - validate_connection()
bool MySQLPool::validate_connection(std::shared_ptr<IDBConnection> connection) {
    return connection->query("SELECT 1") != nullptr;  // 使用普通查询验证连接
}

// mysql_pool.cpp - get_connection()
auto connection = it->second;
if (!validate_connection(connection)) {  // 在返回连接之前执行验证
    ...
}
```

**执行流程**：
1. `get_connection()` 被调用
2. 调用 `validate_connection()` 验证连接
3. `validate_connection()` 执行 `SELECT 1` 普通查询
4. 验证通过，返回连接给调用者
5. 调用者使用该连接执行 prepared statement

**问题根源**：
MySQL C API 的 prepared statement 是与连接绑定的。当在同一连接上执行普通查询（如 `SELECT 1`）后，prepared statement 的状态可能会被重置或失效。虽然 `mysql_stmt_execute()` 返回成功，但实际上插入的参数值丢失，导致数据库中存储的是空值。

### 3. 测试验证

**测试步骤**：
1. 修改代码使用 prepared statement
2. 发送测试邮件
3. 查询数据库

**测试结果**：
```
affected rows: 1  // prepared statement 执行成功
```
```sql
SELECT * FROM mails;
+----+--------+-----------+---------+--------------------------+--------+
| id | sender | recipient | subject | body_path                | status |
+----+--------+-----------+---------+--------------------------+--------+
| 10 |        |           |         |                          |      2 |
+----+--------+-----------+---------+--------------------------+--------+
```
所有字符串字段为空，但 status 字段正确（可能是默认值或随机值）。

**使用 escape_string() 方案测试结果**：
```sql
SELECT * FROM mails;
+----+------------+------------+------------------+--------------------------+--------+
| id | sender     | recipient  | subject          | body_path                | status |
+----+------------+------------+------------------+--------------------------+--------+
| 11 | asd@acc    | asd@svd    | SMTP Test Mail   | mail/1767610056_asd@acc  |      2 |
+----+------------+------------+------------------+--------------------------+--------+
```
数据完全正确。

## 解决方案

### 方案一：使用 escape_string() + 直接 SQL（推荐）

**优点**：
- ✅ 与连接池的验证机制兼容
- ✅ 代码简单，易于调试
- ✅ 提供足够的 SQL 注入防护
- ✅ 性能差异在实际使用中可忽略

**缺点**：
- ❌ 每次需要字符串转义
- ❌ SQL 语句需要每次解析

**实现**：
```cpp
std::string sql_direct = "INSERT INTO mails (sender, recipient, subject, body_path, status) VALUES ('"
    + mysql_connection->escape_string(mail_data.from) + "', '"
    + mysql_connection->escape_string(recipient) + "', '"
    + mysql_connection->escape_string(mail_data.subject) + "', '"
    + mysql_connection->escape_string(body_path) + "', "
    + std::to_string(mail_data.status) + ")";
success = mysql_connection->execute(sql_direct);
```

### 方案二：为 prepared statement 创建专门的连接池（可选）

**优点**：
- ✅ 避免 prepared statement 状态被普通查询干扰
- ✅ 更好的性能（SQL 语句缓存）

**缺点**：
- ❌ 需要维护两个连接池
- ❌ 增加系统复杂度
- ❌ 资源利用率降低

**实现思路**：
```cpp
// 创建不执行验证查询的连接池
auto stmt_pool = create_statement_pool();
stmt_pool->set_validation_enabled(false);
```

## 总结

### 问题本质

MySQL C API 的 prepared statement 与连接池的验证机制存在兼容性问题。当连接池使用普通查询（`SELECT 1`）验证连接后，prepared statement 的状态会被重置，导致参数值丢失。

### 建议

对于当前邮件系统：
1. **使用方案一（escape_string()）**：简单、可靠、性能足够
2. **保留方案二作为未来优化方向**：如果未来需要处理高频批量操作，可以考虑实现专门的 prepared statement 连接池

### 经验教训

1. **异步任务与连接池交互时**，需要特别注意连接状态的时序问题
2. **日志调试的重要性**：通过详细日志才能准确定位问题
3. **实际测试优于理论分析**：prepared statement 在理论上更优，但在实际环境中可能存在隐藏问题
