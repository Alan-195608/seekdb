-- Run on two disposable instances and diff the complete client output.
SET NAMES utf8mb4;
CREATE DATABASE ik_dispatch_regression;
USE ik_dispatch_regression;

SELECT TOKENIZE('', 'ik');
SELECT TOKENIZE('南京市长江大桥，三公斤苹果，１２３号，A-12，かな 한글 𠮷 😀', 'ik');
SELECT TOKENIZE('南京市长江大桥，三公斤苹果，１２３号，A-12，かな 한글 𠮷 😀', 'ik',
                '[{"output":"all"},{"additional_args":[{"ik_mode":"max_word"}]}]');
SELECT TOKENIZE(CONCAT(REPEAT('南京市长江大桥，A-12，', 100), '全文索引'), 'ik');

CREATE TABLE docs (
  id INT PRIMARY KEY,
  smart_body TEXT,
  max_body TEXT
);
INSERT INTO docs VALUES
  (1, '南京市长江大桥 三公斤苹果 A-12', '南京市长江大桥 三公斤苹果 A-12'),
  (2, '长江大桥 全文索引 𠮷', '长江大桥 全文索引 𠮷'),
  (3, '数据库 かんじ 한글 😀', '数据库 かんじ 한글 😀'),
  (4, '', ''),
  (5, NULL, NULL);
CREATE FULLTEXT INDEX idx_smart ON docs(smart_body) WITH PARSER ik;
CREATE FULLTEXT INDEX idx_max ON docs(max_body) WITH PARSER ik
  PARSER_PROPERTIES=(ik_mode="max_word");
SELECT id FROM docs WHERE MATCH(smart_body) AGAINST('长江大桥') ORDER BY id;
SELECT id FROM docs WHERE MATCH(max_body) AGAINST('长江大桥' IN BOOLEAN MODE) ORDER BY id;
SELECT id FROM docs WHERE MATCH(max_body) AGAINST('全文索引 A-12') ORDER BY id LIMIT 2;
UPDATE docs SET smart_body = '全文索引 A-12', max_body = '全文索引 A-12' WHERE id = 3;
DELETE FROM docs WHERE id = 2;
SELECT id FROM docs WHERE MATCH(smart_body) AGAINST('全文索引') ORDER BY id;
SELECT id FROM docs WHERE MATCH(max_body) AGAINST('全文索引' IN BOOLEAN MODE) ORDER BY id;
DROP DATABASE ik_dispatch_regression;
