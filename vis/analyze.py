#!/usr/bin/env python3

from contextlib import closing
from pathlib import Path
from subprocess import Popen, PIPE
import argparse
import re
import sqlite3


class DB:

  SQL_INIT = """
    CREATE TABLE IF NOT EXISTS runs (
      id INTEGER PRIMARY KEY ASC,
      prog TEXT,
      mode TEXT,
      run INTEGER,
      utime_ns INTEGER(8),
      stime_ns INTEGER(8),
      wtime_ns INTEGER(8),
      max_rss  INTEGER(8),
      UNIQUE (prog, mode, run));
    CREATE INDEX IF NOT EXISTS runs_progmoderun_idx ON runs(prog, mode, run);

    CREATE TABLE IF NOT EXISTS allocs (
      id INTEGER PRIMARY KEY ASC,
      run_id INTEGER REFERENCES runs(id),
      from_ns INTEGER(8),
      to_ns INTEGER(8),
      base UNSIGNED INTEGER(8),
      size UNSIGNED INTEGER(8));
    CREATE INDEX IF NOT EXISTS allocs_runid_idx ON allocs(run_id);
    CREATE INDEX IF NOT EXISTS allocs_addr_idx ON allocs(base, size);

    CREATE TABLE IF NOT EXISTS access (
      run_id INTEGER REFERENCES runs(id),
      at_ns INTEGER(8),
      addr UNSIGNED INTEGER(8),
      is_write INTEGER);
    CREATE INDEX IF NOT EXISTS access_runid_idx ON access(run_id);
    CREATE INDEX IF NOT EXISTS access_addr_idx ON access(addr);
  """

  # SQL_RUN = """
  #   INSERT INTO runs (prog, mode, run, utime_ns, stime_ns, wtime_ns, max_rss)
  #   VALUES (?, ?, ?, ?, ?, ?, ?)
  #   ON CONFLICT DO UPDATE SET utime_ns = COALESCE(excluded.utime_ns, utime_ns),
  #       stime_ns = COALESCE(excluded.stime_ns, stime_ns),
  #       wtime_ns = COALESCE(excluded.wtime_ns, wtime_ns),
  #       max_rss = COALESCE(excluded.max_rss, max_rss)
  #   RETURNING id;
  # """
  SQL_RUN_CHECK = """
    SELECT id FROM runs WHERE prog = ?1 AND mode = ?2 and run = ?3;
  """
  SQL_RUN_INSERT = """
    INSERT INTO runs (prog, mode, run, utime_ns, stime_ns, wtime_ns, max_rss)
    VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7);
  """
  SQL_RUN_UPDATE = """
    UPDATE runs
    SET utime_ns = COALESCE(?2, utime_ns),
        stime_ns = COALESCE(?3, stime_ns),
        wtime_ns = COALESCE(?4, wtime_ns),
        max_rss = COALESCE(?5, max_rss)
    WHERE id = ?1;
  """

  SQL_ALLOC_CHECK = """
    SELECT id, from_ns, to_ns, base, size FROM allocs
    WHERE run_id = ?1
      AND base = ?3
      AND to_ns >= ?2
      AND (from_ns IS NULL OR from_ns < ?2)
    ORDER BY to_ns ASC
    LIMIT 1;
  """
  SQL_ALLOC_INSERT = """
    INSERT INTO allocs (run_id, from_ns, to_ns, base, size)
    VALUES (?1, ?2, NULL, ?3, ?4);
  """
  SQL_ALLOC_UPDATE = """
    UPDATE allocs
    SET from_ns = ?2, size = ?3
    WHERE id = ?1;
  """

  SQL_FREE_CHECK = """
    SELECT id, from_ns, to_ns, base, size FROM allocs
    WHERE run_id = ?1
      AND base = ?3
      AND from_ns <= ?2
      AND (to_ns IS NULL OR to_ns > ?2)
    ORDER BY from_ns DESC
    LIMIT 1;
  """
  SQL_FREE_INSERT = """
    INSERT INTO allocs (run_id, from_ns, to_ns, base, size)
    VALUES (?1, NULL, ?2, ?3, NULL);
  """
  SQL_FREE_UPDATE = """
    UPDATE allocs
    SET to_ns = ?2
    WHERE id = ?1;
  """

  SQL_ACCESS = """
    INSERT INTO access (run_id, at_ns, addr, is_write)
    VALUES (?, ?, ?, ?);
  """

  SQL_TIMESTAMP_GET = """
    WITH mintimes(at_ns) AS (
      SELECT MIN(at_ns) FROM access WHERE run_id = ?1
      UNION ALL
      SELECT MIN(from_ns) FROM allocs WHERE run_id = ?1
      UNION ALL
      SELECT min(to_ns) FROM allocs WHERE run_id = ?1)
    SELECT MIN(at_ns) FROM mintimes;
  """

  SQL_TIMESTAMP_UPDATE_ACCESS = """
    UPDATE access
    SET at_ns = at_ns - ?2
    WHERE run_id = ?1;
  """

  SQL_TIMESTAMP_UPDATE_ALLOCS = """
    UPDATE allocs
    SET from_ns = from_ns - ?2,
        to_ns = to_ns - ?2
    WHERE run_id = ?1;
  """


  def __init__(self, db_file):
    self._db = sqlite3.connect(db_file)
    self._db.executescript(type(self).SQL_INIT)

  @property
  def version(self):
    return self._db.sqlite_version

  def add_run(self, prog, mode, run, utime_ns=None, stime_ns=None, wtime_ns=None, max_rss=None):
    # print('add_alloc({},{},{},{},{},{},{})'.format(prog, mode, run, utime_ns, stime_ns, wtime_ns, max_rss))
    cur = self._db.execute(type(self).SQL_RUN_CHECK, (prog, mode, run))
    row = cur.fetchone()
    if row is not None:
      row_id = row[0]
      self._db.execute(type(self).SQL_RUN_UPDATE, (row_id, utime_ns, stime_ns, wtime_ns, max_rss))
      self._db.commit()
      return row_id
    else:
      self._db.execute(type(self).SQL_RUN_INSERT, (prog, mode, run, utime_ns, stime_ns, wtime_ns, max_rss))
      cur = self._db.execute(type(self).SQL_RUN_CHECK, (prog, mode, run))
      row = cur.fetchone()
      self._db.commit()
      return row[0] if row is not None else None

  def add_alloc(self, run_id, at_ns, base, size):
    # print('add_alloc({},{},{},{})'.format(run_id, at_ns, base, size))
    cur = self._db.execute(type(self).SQL_ALLOC_CHECK, (run_id, at_ns, base))
    row = cur.fetchone()
    if row is not None:
      id, from_ns, to_ns, pre_base, pre_size = row
      # print(' updating {:d}:   {} - {} ({} @{})'.format(id, from_ns, to_ns, pre_size, pre_base))
      self._db.execute(type(self).SQL_ALLOC_UPDATE, (id, at_ns, size))
      if from_ns is not None:
        # print(' re-adding {} - {}  ({} @{})'.format(from_ns, None, pre_size, pre_base))
        self._db.execute(type(self).SQL_ALLOC_INSERT, (run_id, from_ns, pre_base, pre_size))
    else:
      # print(' adding {} - {}  ({} @{})'.format(at_ns, None, size, base))
      self._db.execute(type(self).SQL_ALLOC_INSERT, (run_id, at_ns, base, size))
    self._db.commit()

  def add_free(self, run_id, at_ns, base):
    # print('add_free({},{},{})'.format(run_id, at_ns, base))
    cur = self._db.execute(type(self).SQL_FREE_CHECK, (run_id, at_ns, base))
    row = cur.fetchone()
    if row is not None:
      id, from_ns, to_ns, pre_base, pre_size = row
      # print(' updating {:d}:   {} - {} ({} @{})'.format(id, from_ns, to_ns, pre_size, pre_base))
      self._db.execute(type(self).SQL_FREE_UPDATE, (id, at_ns))
      if to_ns is not None:
        # print(' re-adding {} - {}  ({} @{})'.format(None, to_ns, pre_size, pre_base))
        self._db.execute(type(self).SQL_FREE_INSERT, (run_id, from_ns, pre_base))
    else:
      # print(' adding {} - {}  ({} @{})'.format(None, at_ns, None, base))
      self._db.execute(type(self).SQL_FREE_INSERT, (run_id, at_ns, base))
    self._db.commit()

  def add_access(self, run_id, at_ns, addr, is_write):
    # print('add_access({},{},{},{})'.format(run_id, at_ns, addr, bool(is_write)))
    self._db.execute(type(self).SQL_ACCESS, (run_id, at_ns, addr, is_write))

  def clean_timestamps(self, run_id):
    cur = self._db.execute(type(self).SQL_TIMESTAMP_GET, (run_id,))
    row = cur.fetchone()
    if row is not None:
      min_ns = row[0]
      self._db.execute(type(self).SQL_TIMESTAMP_UPDATE_ACCESS, (run_id, min_ns))
      self._db.execute(type(self).SQL_TIMESTAMP_UPDATE_ALLOCS, (run_id, min_ns))
      self._db.commit()

  def commit(self):
    self._db.commit()

  def close(self):
    self._db.commit()
    self._db.close()

def sgx64(x):
  mask = 1 << 63
  return x | ~(mask - 1) if x & mask else x

TIME_PAT = re.compile(r"(\d+(?:\.\d+)?)user.*?(\d+(?:\.\d+)?)system.*?(?:(\d):)?(\d):(\d+(?:\.\d+)?)elapsed.*?(\d+)maxresident")
def add_run(db, path, mr):
  prog = mr.group(1)
  mode = mr.group(2)
  run = int(mr.group(3))
  utime_ns = None
  stime_ns = None
  wtime_ns = None
  max_rss = None
  time_file = path / 'time.log'
  if time_file.is_file():
    if mt := TIME_PAT.search(time_file.read_text()):
      # print(mt.groups())
      utime_ns = int(float(mt.group(1)) * 1000000000)
      stime_ns = int(float(mt.group(2)) * 1000000000)
      wtime_ns = (int(mt.group(3)) if mt.group(3) is not None else 0) * 1000000000 * 60 * 60
      wtime_ns += int(mt.group(4)) * 1000000000 * 60
      wtime_ns += int(float(mt.group(5)) * 1000000000)
      max_rss = int(mt.group(6))
  return db.add_run(prog, mode, run, utime_ns, stime_ns, wtime_ns, max_rss), run == 1

ALLOC_FILE_PAT = re.compile(r"^alloc_(\d+)_(\d+).log")
ALLOC_PAT = re.compile(r"^\s*([+-])(\d+(?:\.\d+)?),([0-9a-fA-F]+),([0-9a-fA-F]+)((?:,\d+\+[0-9a-fA-F]+)*)\s*$")
def add_allocs(db, run_id, path):
  idx = 0
  mod = 5
  def printState(mode):
    print('{:s}> Reading allocation: {:d}'.format('' if mode == 0 else '\033[G\033[K', idx), end='\n' if mode == 2 else '', flush=True)
  for alloc_file in path.glob('alloc_*.log'):
    if alloc_file.is_file():
      mf = ALLOC_FILE_PAT.match(alloc_file.name)
      tid = None
      try:
        tid = int(mf.group(2))
      except:
        pass
      with alloc_file.open('r') as stream:
        printState(0)
        for line in stream:
          if ma := ALLOC_PAT.match(line):
            idx += 1
            if idx % mod == 0:
              printState(1)
            at_ns = int(float(ma.group(2)) * 1000000000)
            addr = sgx64(int(ma.group(3), 16))
            size = int(ma.group(4), 16)
            stack = ma.group(5) and ma.group(5)[1:]
            # TODO-lw use tid and stack
            if ma.group(1) == '+':
              db.add_alloc(run_id, at_ns, addr, size)
            else:
              db.add_free(run_id, at_ns, addr)
        printState(2)
        db.commit()

ACCESS_PAT = re.compile(r"(\d+(?:\.\d+)?):\s*\"?(.*?)(?::p+)?\"?:\s*([0-9a-fA-F]+)")
def add_access(db, run_id, path):
  idx = 0
  mod = 1000
  def printState(mode):
    print('{:s}> Reading access: {:d}'.format('' if mode == 0 else '\033[G\033[K', idx), end='\n' if mode == 2 else '', flush=True)
  access_file = path / 'access.dat'
  if access_file.is_file():
    proc = Popen('perf script -i {} --ns -F "time,event,addr"'.format(access_file), shell=True, stdout=PIPE, text=True)
    # might use --reltime, but then async with alloc
    printState(0)
    for line in proc.stdout:
      if ma := ACCESS_PAT.match(line):
        idx += 1
        if idx % mod == 0:
          printState(1)
        at_ns = int(float(ma.group(1)) * 1000000000)
        is_write = 'r20016' in ma.group(2)
        addr = sgx64(int(ma.group(3), 16))
        db.add_access(run_id, at_ns, addr, is_write)
    printState(2)

RUN_PAT = re.compile(r"(.*)\.(\w+)\.(\d+)")
def main(args):
  if not args.result_dir.is_dir():
    print('Result directory "{}" must exist'.format(args.result_dir))
    return
  if args.db_file.exists() and not args.db_file.is_file():
    print('DB file "{}" must be a regular file'.format(args.result_dir))
    return

  with closing(DB(args.db_file)) as db:
    print('SQLite {}'.format(sqlite3.version))
    for path in args.result_dir.iterdir():
      if path.is_dir():
        if m := RUN_PAT.fullmatch(path.name):
          run_id,first = add_run(db, path, m)
          print('Processing run {:d} at {}'.format(run_id, path))
          if first or args.all:
            add_allocs(db, run_id, path)
            add_access(db, run_id, path)
            print('> Normalizing timestamps')
            db.clean_timestamps(run_id)

  # with closing(DB(args.db_file)) as db:
  #   run_id = db.add_run('test', 'test', 1)
  #   db.add_alloc(run_id, 100, 0xA000, 0x10)
  #   db.add_free(run_id,  120, 0xA000)
  #   db.add_alloc(run_id, 110, 0xB000, 0x18)
  #   db.add_free(run_id,  200, 0xA000)
  #   db.add_free(run_id,  160, 0xA000)
  #   db.add_free(run_id,  150, 0xB000)
  #   db.add_alloc(run_id, 130, 0xA000, 0x20)
  #   db.add_alloc(run_id, 190, 0xB000, 0x28)
  #   db.add_alloc(run_id, 150, 0xA000, 0x30)
  #   db.add_free(run_id,  140, 0xA000)

if __name__ == '__main__':
  parser = argparse.ArgumentParser()
  parser.add_argument('-i', '--result-dir', type=Path, required=True)
  parser.add_argument('-o', '--db-file', type=Path, required=True)
  parser.add_argument('--all', action='store_true')
  main(parser.parse_args())
