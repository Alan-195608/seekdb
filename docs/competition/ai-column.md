# Asynchronous AI columns

AI columns run model calls after the business transaction commits. INSERT, changes to referenced input columns, and primary-key changes invalidate the affected results and persist tasks in the same transaction. Rollback removes both the invalidation and queued work. Unrelated-column updates retain the existing result.

Definitions and hidden state/request/fingerprint columns survive schema reloads. The DDL resolver validates model types, embedding dimensions, ordinary-column dependencies, and asynchronous options; single-option WITH clauses are accepted. Business SQL cannot assign an AI result column directly.

The native worker claims committed tasks with durable owner/generation leases, evaluates the existing AI SQL functions with scoped result-type instructions, and caches terminal results. It publishes all still-current results for one row/request group and removes their tasks in one transaction. Both request IDs and definition versions fence stale results. SQL NULL, JSON, integer bounds, vector dimensions and finite vector elements are checked before publication. Normal SQL writes maintain the vector index during invalidation and publication.

`ALTER TABLE ... RETRY AI COLUMN ... WHERE ...` invalidates exactly the named column and matching rows. Bare `status` denotes that AI column's state; quoted `status` denotes a business column. READY rows can be retried. The session `ai_column_strict_read` setting checks the entire referenced AI column before a scan, including SELECT, UPDATE and DELETE; ordinary-column access remains usable.

## Native validation

On Linux, the final combined source was compiled with the repository CMake toolchain and explicit `-j8`. Low disk space required linking the test executable in a dedicated memory directory, then stripping debug symbols into the executable filesystem. No application-layer substitute was used.

All five supplied `tools/deploy/mysql_test/test_suite/ai_column` cases passed in mysqltest verify mode with unchanged `.test` and `.result` files:

- `ai_column_ddl`
- `ai_column_t1_dml`
- `ai_column_worker`
- `ai_column_retry_strict`
- `ai_column_vector_index`

The same combined binary also passed seven AI-function/File-SQL cases: `ai_prompt`, `ai_function_enhancement`, `ai_model_ddl`, `ai_model_lower_case_name_mysql0`, `ai_model_lower_case_name_mysql1`, `file_sql_basic`, and `file_sql_edge_cases`. The test account required the grant option for the model privilege test; missing environment privileges were corrected without editing expected results.

Eleven extra native DDL checks covered single-option definitions, rejection of forward AI dependencies in multi-action ALTER, and deletion of seeded pending tasks on column/table DROP. Strict-read probes also rejected pending AI reads with constant-false filters, LIMIT 0 and missing primary keys while allowing ordinary-column reads.

## Limits

Task synchronization currently scans the table and compares input fingerprints, so business-write cost grows with table size. Worker execution is serial; task ownership is fenced, but multi-worker throughput and lease takeover under long-running calls were not benchmarked. The supplied fault cases passed; response-loss gateway experiments and hidden platform scoring were not run. These results establish local functionality, not a guaranteed contest score.

See [AI-function and File-SQL validation](seekdb-validation.md) for their separate known limits. Qoder and MiniMax contributed earlier implementation and experiments; Codex completed integration and native verification after their quota was exhausted. The user performs final contest submission.
