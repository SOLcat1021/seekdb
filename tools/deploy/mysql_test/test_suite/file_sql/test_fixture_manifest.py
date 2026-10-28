from pathlib import Path
import os
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[5]
include = root / 'tools/deploy/mysql_test/test_suite/file_sql/include/copy_file_sql_data.inc'
source = include.read_text()
with tempfile.TemporaryDirectory(prefix='seekdb-fixture-manifest-') as tmp:
    base = Path(tmp)
    data = base / 'mysql_test/test_suite/file_sql/data'
    data.mkdir(parents=True)
    env = dict(os.environ)
    names = ['basic.csv', 'basic.jsonl', 'invalid.txt', 'simple.parquet.b64']
    for name in names:
        relative = 'tools/deploy/mysql_test/test_suite/file_sql/data/' + name
        shutil.copyfile(root / relative, data / name)
        (data / ('._' + name)).write_bytes(b'archive metadata')
    (data / 'editor-backup.csv').write_text('unrelated')
    bins = base / 'bin'
    bins.mkdir()
    (bins / 'ssh').write_text('#!/bin/sh\nshift\nexec sh -c "$1"\n')
    (bins / 'scp').write_text('#!/usr/bin/env python3\nimport sys,shutil\na=[v for v in sys.argv[1:] if v!="-q"]\ndest=a[-1].split(":",1)[1]\nfor src in a[:-1]: shutil.copy(src,dest)\n')
    for script in bins.iterdir(): script.chmod(0o755)
    destination = base / 'observer/data_for_mysqltest_file_sql'
    command = next(line for line in source.splitlines() if line.startswith('let $FILE_SQL_COPY_CMD = ')).split(' = ', 1)[1].rstrip(';')
    command = command.replace('$FILE_SQL_HOST', 'fixture-test').replace('$FILE_SQL_DATA_DIR', str(destination))
    env['PATH'] = str(bins) + ':' + env['PATH']
    subprocess.run(['bash', '-c', command], cwd=base, env=env, check=True)
    actual = sorted(p.name for p in destination.iterdir())
    assert actual == ['basic.csv', 'basic.jsonl', 'invalid.txt', 'simple.parquet'], actual
    print('fixture manifest: PASS (four sidecars and unrelated backup excluded)')
