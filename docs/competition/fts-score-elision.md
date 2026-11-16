# Delivery status after benchmark failure

The score-elision optimizer change has been withdrawn. The submission now
retains the original optimizer and relevance execution path, alongside the
IK and BENG tokenization changes. Static inspection found that the dormant
posting-only path still accesses absent document/token aggregation objects
in the current executor. It must not be enabled through an optimizer flag
alone. No further test or build was executed, per user instruction.

The following records the withdrawn design, not active submission behavior.

# Full-text relevance elision and BENG normalization

Based on competition revision `5bce9e06d1281347ad61344e4b01fab9158c58a2`.
Benchmark inputs, SQL, timing, scoring, baseline and CI remain unchanged.

## Retrieval

`prepare_text_retrieval_info` always requests relevance computation because its
old score-usage check is commented out. Even COUNT with a bare MATCH predicate
therefore requests BM25 statistics and scoring.

The new optimization handles only one pushed BOOL(MATCH), unweighted natural
language mode, outside hybrid vector scans. It checks every statement relation
expression, including SELECT, ORDER BY, GROUP BY and HAVING. Truth context is
propagated only through WHERE AND/OR/BOOL nodes. Any numeric MATCH use, or an
optional null child, conservatively retains scoring. The actual target must be
seen. Functional lookup, multiple MATCH filters and other search modes retain
their existing paths.

A matching unweighted natural-language token has positive relevance because
`ObExprBM25::query_token_weight` clamps IDF to positive epsilon. Posting union
therefore implements the truth predicate. Both relevance calculation and its
redundant pushed predicate are removed together, so TAAT never requests a
missing relevance expression solely to evaluate that predicate. Existing
posting retrieval retains document deduplication, snapshots and limit/offset.

## BENG

UTF-8 ASCII advances one byte instead of invoking multibyte decoding. ASCII
case conversion is limited to default/Unicode 5.2 case tables; other tables
(including Turkish) retain generic conversion. Already lowercase ASCII words
borrow input memory until the next token. The existing BENG iterator copies
the returned token before the word map retains it. Uppercase words use the
normalization arena. Non-ASCII tokens retain the original conversion.

The branch also includes earlier IK decode-once, processor-input reuse,
terminal-trie pruning and unused stop-dictionary initialization changes.

## Validation boundary

The earlier BENG/IK candidate compiled and passed BENG, IK, tokenize_function
and cn_word SQL regressions. Its 2,112 complete BENG TOKENIZE results matched
a separately rebuilt baseline over the four exposed collations, including
word frequencies. IK classification passed 8,046,468 differential cases under
ASan/UBSan. These checks preceded the optimizer change.

At the user's request the final optimizer change has NOT been compiled or
executed. User-side CI must establish its correctness and performance. No
competition score or mathematical global optimum is claimed. Earlier timing
results do not validate this final change.
