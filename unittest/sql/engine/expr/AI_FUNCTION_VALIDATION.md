# AI function integration checks

Run `python3 unittest/sql/engine/expr/ai_function_mock.py 19125` on the database host, then execute `ai_function_integration.sql` against a fresh test instance. The SQL creates only models/endpoints prefixed `task1_` and tables inside `seekdb_task_checks`.

Validated against a seekdb binary compiled from the competition branch plus this change:

- JSON array/object and nested AI_PROMPT arguments, Unicode, TEXT/LONGTEXT.
- A 100,000-character argument referenced three times in a completion prompt; result length 300,006, including the mock's six-character reply prefix.
- OpenAI completion with SQL NULL configuration and native JSON configuration.
- Default and explicit embedding dimensions, SQL NULL propagation, table scans, filters and LIMIT.
- A single HTTP request carrying three non-NULL table rows; the mock reverses response indices and SQL still returns each vector on its original row.
- Ollama endpoint creation/alteration with an empty key, case-insensitive provider names, non-streaming completion and native embedding.
- String/object reranking, original object IDs, descending relevance and original input order for tied scores.
- Rejection of zero, negative and fractional dimensions, an overflowing placeholder index, and a non-object completion configuration; subsequent ordinary SQL succeeds.

The mock uses deterministic arithmetic rather than inference. These checks do not establish a platform score or substitute for the organizer's hidden cases.
