# Remove only the edge-case fixture directory created by copy_file_sql_edge_data.inc.
connect (file_sql_edge_cleanup,$OBMYSQL_MS0,admin,$OBMYSQL_PWD,test,$OBMYSQL_PORT);
connection file_sql_edge_cleanup;

--disable_query_log
let $FILE_SQL_HOST = query_get_value('select host_ip()',host_ip(), 1);
let $FILE_SQL_EDGE_DIR = $OBSERVER_DIR/data_for_mysqltest_file_sql_edge;
let $FILE_SQL_EDGE_CLEANUP_CMD = ssh $FILE_SQL_HOST 'rm -rf "$FILE_SQL_EDGE_DIR"';
exec $FILE_SQL_EDGE_CLEANUP_CMD;
--enable_query_log

disconnect file_sql_edge_cleanup;
connection default;
