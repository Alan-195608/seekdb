# 全文索引调优：IK 字符分类与处理器调度

## 基线与范围

基于比赛源码 `5bce9e06d1281347ad61344e4b01fab9158c58a2`。
只修改 IK 分词内核，不修改基准数据、SQL、计时、评分规则或固定基线。
同一分词路径用于 TOKENIZE、IK 索引构建以及 MATCH 查询词解析。

## 瓶颈依据与修改

`ObFTCharUtil::do_classify` 原先依次调用分类包装函数，每个函数重新解码相同字符。
中文字符需要三次解码，其他 CJK 或无用字符需要四次；UTF16 路径最多六次。
现在只解码一次，按原顺序调用原有 Unicode 分类谓词。全角字母、数字仍优先于
OTHER_CJK；代理项检查仍限于 UTF16/UTF16LE；错误码和解码失败行为保持原状。
这不改变词典、停用词、分词模式或字符归一化。

`process_next_batch` 已经读取并检查当前字符、字节长度和类型，而四个处理器的
`process(ctx)` 又各调用两次读取函数。现在把已验证的参数传给 `do_process`，
每个字符减少八次访问函数调用。处理器顺序及遇错停止逻辑保留。已检查四个实际
处理器：它们不会推进上下文游标，只有外层循环在四个处理器完成后调用 step_next。

## 已执行验证

运行：

```sh
python3 unittest/fts/test_ik_classification.py --sanitize
python3 unittest/fts/test_ik_classification.py --benchmark
```

差分测试直接编译仓库实际解码器、Unicode 谓词以及修改前后的字符分类实现。
为脱离服务器运行时，测试提供最小日志、状态码与字符集分派适配层；它不是完整
服务器编译或 SQL 集成测试。测试覆盖所有有效 Unicode 码点、逐字节截断、全部
两字节组合及附加非法字节、空指针、UTF8/UTF16/UTF16LE 和不支持的字符集分派。
另有独立的 ASCII、中文、全角字母/数字、日文、标点与 emoji 预期类别断言。

macOS / Apple Clang / C++17 / -O2 本地结果：

- 8,046,468 个差分用例通过。
- AddressSanitizer 与 UndefinedBehaviorSanitizer 检查通过。
- 混合中文及 ASCII 字符分类微基准，五组交替执行顺序：

| 组 | 基线 ms | 优化 ms | 耗时下降 |
|---|---:|---:|---:|
| 1 | 16.148 | 7.394 | 54.21% |
| 2 | 14.904 | 7.291 | 51.08% |
| 3 | 14.658 | 7.128 | 51.37% |
| 4 | 15.104 | 7.140 | 52.73% |
| 5 | 15.888 | 7.159 | 54.94% |

这是字符分类函数的局部测量，不是整个分词、索引构建或查询的加速比例，也不是
比赛评分。对 BENG 不预期有直接改善；实际收益由 IK 分词在总耗时中的占比决定。

## 集成回归与正式评测待执行

新增 mysqltest：`fts_index.ik_hotpath_regression`，覆盖 smart/max_word 两种模式、
混合字符、跨批次末尾 token、事后建立索引以及插入/更新/删除后的完整命中 ID。
应与现有 `fts_index.cn_word`、`fts_index.tokenize_function` 一起在真实实例运行。
该 SQL 回归尚未在本地执行：本机没有已编译的 seekdb 和 MySQL 客户端。

正式性能对比应在同一台机器上，分别构建基线与提交版本，用全新报告文件运行：

```sh
LABEL=local ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 \
  SAMPLES=3 WARMUP=30 SKIP_LOAD=0 OUTPUT=/tmp/fts-new-report.txt \
  bash tools/benchmark/fts_large_bench.sh
python3 tools/benchmark/fts_large_bench_score.py /tmp/fts-new-report.txt
```

脚本会重建 `fts_large_bench` 数据库，需使用专用测试实例。
当前比赛源码不包含说明中提到的 `.github/workflows/vldb.yml`，只有其结果评论
workflow；因此提交平台统一评测前，无法从此仓库自行运行该官方 workflow。
完整构建、SQL 回归及官方九项指标成绩均未在此文档中声称通过。
