# seekdb 比赛实现与验证

本分支整合 AI 函数增强、文件即数据库与既有全文索引实现。AI 列的异步内核改动在独立工作区开发，尚未集成，不能把已有语法或元数据草稿视为完整功能。正式评测由用户提交，当前没有可确认的新平台分数。

## AI 函数增强

复用已有实现，覆盖 AI_PROMPT 的 JSON、重复参数与长文本；OpenAI Completion/Embedding；Ollama 原生协议及空 key；Rerank 原 ID、排序与乱序响应；跨行 Embedding 的批量、NULL 和原行映射。

真实 SQL 复验使用确定性 Mock，不调用真实模型。详细已有检查见 `unittest/sql/engine/expr/AI_FUNCTION_VALIDATION.md`；补充的 `ai_function_mock_8000.py` 提供官方公开用例需要的 URL 路径和默认维度，按请求内容计算结果，并故意逆序返回 embedding 索引。

## 文件即数据库修复

- CSV/JSONL 的 DATETIME 列保存实际小数秒精度。无小数值按普通 DATETIME 输出，有小数值保留精度。
- 日期识别要求完整消费输入，`2026-08-01 10:11:12.1junk` 保持 VARCHAR 原文，不被错误推入 DATETIME 转换路径。
- Parquet timestamp 列将其时间精度传给 SQL 列元数据。
- `COPY read_csv(...) TO path` 通过 seekdb 原生 SELECT 计划导出文件表。

普通无效日期字符串仍保留文本语义；原闰年判断本来正确，本次相关用例属于边界覆盖。未修改官方测试与期望输出。

## 已执行检查

Linux 真实二进制编译成功，使用显式 `-j8` 并以共享锁串行安排重型编译。源码与测试实例对应构建树的三份修改源码 SHA256 一致。

Codex 在真实实例上以 mysqltest verify 模式独立复跑：

| 用例 | 结果 |
| --- | --- |
| 官方 `file_sql_basic` | 通过 |
| 新增 `file_sql_edge_cases` | 通过 |
| 官方 `ai_function_enhancement` | 通过 |
| 官方 `ai_prompt` | 通过 |

MiniMax 另外执行了 51 条断言式 SQL 检查、8 条安全和资源边界检查、文件/Schema 变化检测，以及 `ai_model_ddl` 和 `ai_model_lower_case_name_mysql1`。这些补充结果由 MiniMax 报告；不应混同为 Codex 已逐项复跑。

新增时间后缀回归在补丁的缺陷版本上失败，修正后通过；Codex 亦重新编译实际 Reader 复现，确认合法小数时间为 DATETIME、带非法后缀的文本为 VARCHAR。

新增回归入口：

- `tools/deploy/mysql_test/test_suite/file_sql/t/file_sql_edge_cases.test`
- 对应 `r/mysql/file_sql_edge_cases.result`、`include/` 和独立 `data_edge/`
- `unittest/sql/engine/basic/make_file_sql_edge_data.py` 可重建字节精确夹具

夹具含 CRLF、嵌入换行、BOM 和无效 UTF-8，必须保留原字节，不能通过 Git 换行转换改变。运行 mysqltest 需使用仓库既有部署方式，设置数据库连接、`OBSERVER_DIR`、`secure_file_priv` 和 Mock 服务；校验使用 `--result-file`，不要以 record 模式替代通过判断。

## 补充真实 SQL 复核

源代码与已编译运行实例一致的 Linux 环境中，官方 mysqltest 使用 verify 模式执行：`ai_prompt`、`ai_function_enhancement`、`ai_model_ddl`、两项模型名称大小写测试、`file_sql_basic`、`file_sql_edge_cases` 共七项通过。额外的 105 条 AI 函数断言、18 条请求协议断言、59 条文件 SQL 断言与16条按顺序修改文件 schema 的检查通过。所有模型调用使用确定性本地 Mock，不使用真实模型效果作为判断。

## 验证范围与局限

- `ai_model_endpoint_ddl.test` 未通过：当前源码不支持测试中的 `max_concurrency` 参数。本次文件 SQL 修复没有扩展并发控制架构。
- `ai_model_lower_case_name_mysql0.test` 已补齐测试环境缺失的原版 include 并通过；mysql0/mysql1 大小写用例均通过。
- COPY 的 delimiter/quote/escape 已有实现与专项运行记录，新增 mysqltest 未逐项覆盖所有组合。
- `ai_embed` 直接作为表扫描投影时合并多行请求；嵌套在其他表达式内时值与行映射正确，但现有表达式框架可能逐行调用，未宣称嵌套表达式也具有相同批量性能。
- 已缓存的文件查询在原文件 schema 改变后明确报错，避免返回旧列结构；当前需要新语句或清理 plan cache 后重新规划，未实现自动失效重规划。
- HTTP 500 在默认查询超时下可能最终表现为 Timeout；延长查询超时后可见实际 HTTP 状态错误。此重试行为沿用原始实现。
- 题面列举的 AI 函数 C++ 单测目标在当前仓库中不存在，因此使用真实 SQL 等价验证；该轮复核未重编译未改动的源码。
- 隐藏用例和平台正式评分尚未执行，本地结果不能保证满分。

## 协作与目录

Qoder、MiniMax 参与实现与测试；额度耗尽后由 Codex 接手实现、独立校验、集成及 GitHub 更新；用户最终提测。生产源码与必要回归进入 Git，原始日志、模型提案、缓存和构建过程保存在各工作区本地忽略目录 `.artifacts/`。
