# Install deterministic edge-case fixtures for the file SQL regression test.
# The fixtures are copied into a dedicated directory inside the observer working
# directory; the include never removes anything outside that directory.
connect (file_sql_edge_setup,$OBMYSQL_MS0,admin,$OBMYSQL_PWD,test,$OBMYSQL_PORT);
connection file_sql_edge_setup;

--disable_query_log
let $FILE_SQL_HOST = query_get_value('select host_ip()',host_ip(), 1);
let $FILE_SQL_EDGE_DIR = $OBSERVER_DIR/data_for_mysqltest_file_sql_edge;
let $FILE_SQL_EDGE_CMD = ssh $FILE_SQL_HOST 'rm -rf "$FILE_SQL_EDGE_DIR" && mkdir -p "$FILE_SQL_EDGE_DIR"' && scp -q $PWD/mysql_test/test_suite/file_sql/data_edge/* $FILE_SQL_HOST:"$FILE_SQL_EDGE_DIR/";
exec $FILE_SQL_EDGE_CMD;
--enable_query_log

disconnect file_sql_edge_setup;
connection default;
