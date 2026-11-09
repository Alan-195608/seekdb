# MiniVectorDB

单机、单进程、单客户端的 C++20 向量数据库。通过标准输入和标准输出工作，诊断信息写入标准错误。运行阶段无需网络。

## 构建与运行

在仓库根目录执行：

```sh
docker build --platform linux/amd64 -t minivectordb:round2 .
docker volume create minivectordb-data
docker run --rm --platform linux/amd64 --network none -i \
  -v minivectordb-data:/data minivectordb:round2
```

原生构建需要支持 C++20 和浮点 `std::from_chars` 的编译器，例如 GCC 14。

```sh
cmake -S minivectordb -B /tmp/minivectordb-build -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/minivectordb-build
/tmp/minivectordb-build/minivectordb --data /tmp/minivectordb-data
python3 minivectordb/verify.py /tmp/minivectordb-build/minivectordb
```

## SQL 示例

空目录启动后逐行发送：

```sql
CREATE TABLE docs (id BIGINT PRIMARY KEY, topic VARCHAR(12), embedding VECTOR(2));
INSERT INTO docs VALUES (1, '数据库', '[0,1]'), (2, '数据库', '[3,4]');
SELECT id, topic FROM docs WHERE id >= 1 ORDER BY id;
CREATE VECTOR INDEX docs_embedding ON docs(embedding);
SELECT id, L2_DISTANCE(embedding, '[0,0]') AS d FROM docs WHERE topic = '数据库' ORDER BY d LIMIT 1;
BEGIN;
UPDATE docs SET embedding = '[1,0]' WHERE id = 1;
COMMIT;
BEGIN;
DELETE FROM docs WHERE id = 1;
ROLLBACK;
QUIT;
```

复用原数据目录重启后，`SELECT * FROM docs WHERE id = 1;` 返回向量 `[1,0]` 的记录。

支持一个 BIGINT 主键、BIGINT、VARCHAR、VECTOR、整行或列顺序重排插入、多行插入、条件 UPDATE 和 DELETE、普通列投影、WHERE 比较与 AND、ORDER BY 和 LIMIT。VARCHAR 按 UTF-8 Unicode 码点计长，向量写入 float32，距离用双精度计算。对排序值相同的记录按主键升序。

## 本地通信协议

公开用例定义了 SQL 和结果语义，完整官方驱动及通信协议仍需核对。本地适配层位于 `main.cpp`，每行接收一条完整 SQL，末尾分号可省略。恢复完成后输出 `READY`。命令响应为以下格式，其中 `\t` 表示制表符：

- `OK\t<受影响行数>`
- `RESULT\t<行数>\t<列数>`，随后为一行列名和指定数量的数据行。列间用制表符分隔，字符串转义反斜杠、制表符、换行、回车和 NUL。
- `ERR\t<错误码>\t<错误信息>`。SQL 错误保留当前会话，存储 I/O 异常写入 stderr 并终止。
- `MODE exact` 和 `MODE ann` 切换向量查询模式，默认为 exact。`QUIT` 或输入结束会回滚未提交事务。

## 存储与检索

- `/data/mini.wal` 保存表定义、索引定义及行修改。内存保存主键到最新磁盘偏移的映射。记录按需读取，向量数据通过文件映射直接参与计算。无排序、无限制查询流式输出行数据。
- 每个事务有一个追加日志帧。修改先写入日志主体，提交时写入帧长度和反码并同步文件，之后才回应成功。语句保存点同时记录撤销项及写入偏移，失败时截断该语句的修改，保留事务中此前成功的语句。
- 启动按顺序重放完整帧，截断末尾未提交帧，重建声明过的向量索引后输出就绪。单目录使用文件锁防止多进程并发写入。
- exact 模式先筛选可见记录，再用堆维护有界 TopK。x86-64 支持运行时 AVX2 选择，标量路径保持同样的双精度计算。索引对向量首维有序排列，ann 模式的升序有界 TopK 在该索引上按单维距离扩展，以当前第 K 个距离为上界剪枝。这一检索仍给出精确结果。过滤条件和主键排序规则同时适用。写入、删除或回滚使索引失效，下次使用时按当前事务视图重建。
- 当前日志为追加格式，未实现历史版本压缩或磁盘外排序。主键映射和已建向量索引的内存用量随记录数增长，事务撤销项随事务修改数增长。首维剪枝在高维或选择性较低的场景可能退化为全扫描。

`verify.py` 默认运行仓库中的首批 5 组官方公开 SQL 断言。通过 `--cases` 指定官方 v4 的 `mysqltest` 目录可运行新版 20 组样例，包括重启、模式切换与错误语义。测试脚本还使用独立暴力枚举检查事务前、事务中和回滚后的 TopK。恢复测试构造跨缓冲区的失败语句，用 SIGKILL 终止含未提交修改的进程，再三次重启检查状态。正式结果以赛事平台评测为准。

本地 Linux x86_64 模拟容器完成以上验证。另一轮在禁用交换空间、192 MiB 内存限制下写入 52,000 行，生成 214,504,233 字节日志，随后以相同限制重启并核对主键查询和精确 TopK。构建使用 Alpine 的签名软件包及版本锁。
