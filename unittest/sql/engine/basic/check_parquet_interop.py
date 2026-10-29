#!/usr/bin/env python3
"""Check the actual C++ scanner against independently generated Arrow fixtures.

Usage: python3 check_parquet_interop.py /absolute/path/to/test_parquet_interop
Requires pyarrow only for generating fixtures, never in the database executable.
"""
import datetime
from pathlib import Path
import subprocess
import sys
import tempfile
import pyarrow as pa
import pyarrow.parquet as pq

rows = [
    [1, 1.25, True, '中文', datetime.date(2026, 1, 1), datetime.datetime(2026, 1, 1)],
    [2, None, False, 'a', None, None],
    [None, -3.0, None, 'a', datetime.date(2024, 2, 29), datetime.datetime(2024, 2, 29, 12, 13, 14)],
    [4, .5, True, None, datetime.date(1970, 1, 1), datetime.datetime(1970, 1, 1)],
    [5, 8.0, False, '😀', datetime.date(1969, 12, 31), datetime.datetime(1969, 12, 31, 23, 59, 59)],
]
names = ['id', 'value', 'enabled', 'text', 'day', 'stamp']
types = [pa.int64(), pa.float64(), pa.bool_(), pa.string(), pa.date32(), pa.timestamp('us')]
table = pa.table({name: pa.array([row[i] for row in rows], typ) for i, (name, typ) in enumerate(zip(names, types))})

def formatted(value):
    if value is None:
        return 'NULL'
    if isinstance(value, bool):
        return 'true' if value else 'false'
    if isinstance(value, datetime.datetime):
        return value.isoformat(sep=' ')
    if isinstance(value, float):
        return format(value, '.17g')
    return str(value)

expected = ''.join('\t'.join(map(formatted, row)) + '\n' for row in rows)
with tempfile.TemporaryDirectory(prefix='file-sql-parquet-', dir='/tmp') as directory:
    for version in ['1.0', '2.0']:
        for codec in ['NONE', 'SNAPPY']:
            for dictionary in [False, True]:
                path = Path(directory) / f'{version}-{codec}-{dictionary}.parquet'
                pq.write_table(table, path, row_group_size=2, data_page_version=version,
                               compression=codec, use_dictionary=dictionary)
                actual = subprocess.run([sys.argv[1], str(path)], capture_output=True, text=True, check=True)
                assert actual.stdout == expected, (path.name, actual.stdout, expected)
                print(path.name, 'passed')
