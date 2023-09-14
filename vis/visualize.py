#!/usr/bin/env python

### Objective: Plot chart of allocations and memory accesses

import argparse
from collections import namedtuple
import io
import logging
import re
import sqlite3
import statistics as stat
from itertools import product
import sys

import matplotlib as mpl
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import matplotlib.patches as patches
import matplotlib.colors as colors
import numpy as np
import pandas as pd
import seaborn as sns

import holoviews as hv
from holoviews import opts


class HeimdallrViz(object):

  def __init__(self, args=None):
    self.args = args or self.parse_args()
    self.db = sqlite3.connect(self.args.db_file)
    self.addr_unit = 1 << self.args.block_bits
    self.addr_min = self.args.block_min and self.args.block_min * self.addr_unit
    self.addr_max = self.args.block_max and self.args.block_max * self.addr_unit
    self.time_unit = self.args.time_unit
    self.time_min = self.args.time_min
    self.time_max = self.args.time_max

    cmap = {
            'red':   [(0.0, 1.0, 1.0), (0.5, 0.5, 0.5), (1.0, 0.2, 0.2)],
            'green': [(0.0, 0.6, 0.6), (0.5, 0.5, 0.5), (1.0, 0.6, 0.6)],
            'blue':  [(0.0, 0.2, 0.2), (0.5, 0.5, 0.5), (1.0, 1.0, 1.0)]
           } if self.args.borderless else {
            'red':   [(0.0, 0.8, 0.8), (0.5, 0.4, 0.4), (1.0, 0.1, 0.1)],
            'green': [(0.0, 0.4, 0.4), (0.5, 0.4, 0.4), (1.0, 0.4, 0.4)],
            'blue':  [(0.0, 0.1, 0.1), (0.5, 0.4, 0.4), (1.0, 0.8, 0.8)]
           }
    self.colormap = colors.LinearSegmentedColormap('accesscolors', cmap)
    self.allocline = np.array([(0.5, 0.8, 0.1, 1.0)])
    self.allocfill = np.array([(0.5, 0.8, 0.1, 0.2)])
    self.boundline = np.array([(0.8, 0.8, 0.8, 1.0)])

  def setup_plot(self):
    self.run_id = self.get_run_id(self.args.run)
    print('Analyzing run {:d} within ({},{})@{}B and ({},{})@{}ns'.format(self.run_id, self.addr_min, self.addr_max, self.addr_unit, self.time_min, self.time_max, self.time_unit))
    self.setup_bounds()
    print('Found {:d} regions {:d} x {:d}B total between 0 and {:,} ns'.format(len(self.blocks), self.end_vblock//self.addr_unit, self.addr_unit, self.end_time))
    # TODO autoscale...
    #self.time_unit = max(self.end_time // 10000, 1)
    #print('Adjusting time_unit to {:,} ns'.format(self.time_unit))

  def parse_args(self):
    parser = argparse.ArgumentParser(
        description='Memory Access Virtualization')
    parser.add_argument('db_file',
        help="heimdallr-pmu results database file (.sqlite)")
    cmd_group = parser.add_mutually_exclusive_group(required=True)
    cmd_group.add_argument('--run',
        help="<prog>.<mode> or <run_id>",
        type=str)
    parser.add_argument('--block-bits',
        help="<n> so that address blocks of 2**<n> byte size are grouped",
        type=int,
        default=12)
    parser.add_argument('--block-min',
        help="begin of the visualized address range in block multiples (see --block-bits)",
        type=int,
        default=0)
    parser.add_argument('--block-max',
        help="end of the visualized address range in block multiples (see --block-bits)",
        type=int,
        default=None)
    parser.add_argument('--time-unit',
        help="timespan in nanoseconds to group accesses by",
        type=int,
        default=1000000)
    parser.add_argument('--time-min',
        help="begin of the visualized timespan in nanoseconds",
        type=int,
        default=None)
    parser.add_argument('--time-max',
        help="end of the visualized timespan in nanoseconds",
        type=int,
        default=None)
    parser.add_argument('--borderless',
        help="draw access markers without white border",
        action='store_true')
    cmd_group.add_argument('--list',
        help="List recorded run configurations",
        action='store_true')
    cmd_group.add_argument('--stat',
        help="Output time statistics for each run configuration",
        action='store_true')
    parser.add_argument('--h')
    return parser.parse_args()

  def list_run_ids(self):
    SQL = """SELECT id, prog, mode, MIN(run) FROM runs GROUP BY prog, mode;"""
    for row in self.db.execute(SQL):
      print('{:3d}: {}.{}.{}'.format(*row))

  def print_run_stats(self):
    SQL = """
      SELECT prog, mode, count(*), group_concat(utime_ns, ';'), group_concat(stime_ns, ';'), group_concat(wtime_ns, ';')
      FROM runs
      GROUP BY prog, mode
      ORDER BY prog, mode;
    """
    val_labels=('utime', 'stime', 'wtime')
    func_labels=('med', 'min', 'max', 'avg', 'std')
    line = ['prog', 'mode', 'reps']
    line.extend('{}_{}'.format(val, func) for val,func in product(val_labels, func_labels))
    print(','.join(line))
    for row in self.db.execute(SQL):
      utimes = list(map(lambda s: float(s)/1e9, row[3].split(';')))
      stimes = list(map(lambda s: float(s)/1e9, row[4].split(';')))
      wtimes = list(map(lambda s: float(s)/1e9, row[5].split(';')))
      vals = (utimes, stimes, wtimes)
      funcs = (stat.median, min, max, stat.mean, stat.stdev)
      line = [row[0], row[1], str(row[2])]
      line.extend('{:.4e}'.format(func(val)) for val,func in product(vals, funcs))
      print(','.join(line))
      # print('{:s},{:s},{:.4e},{:.4e},{:.4e},{:.4e},{:.4e},{:.4e},{:.4e},{:.4e},{:.4e},{:.4e},{:.4e},{:.4e},{:.4e},{:.4e},{:.4e}'.format(row[0], row[1],
      #         stat.median(utimes), min(utimes), max(utimes), stat.mean(utimes), stat.stdev(utimes),
      #         stat.median(stimes), min(stimes), max(stimes), stat.mean(stimes), stat.stdev(stimes),
      #         stat.median(wtimes), min(wtimes), max(wtimes), stat.mean(wtimes), stat.stdev(wtimes)))

  def get_run_id(self, run_spec):
    try:
      run_id = int(run_spec)
      SQL = """SELECT id FROM runs WHERE id = ?1 ORDER BY run;"""
      row = self.db.execute(SQL, (run_id,)).fetchone()
      return row[0]
    except ValueError:
      try:
        prog,mode = run_spec.rsplit('.', 1)
        SQL = """SELECT id FROM runs WHERE prog = ?1 AND mode = ?2;"""
        row = self.db.execute(SQL, (prog, mode)).fetchone()
        return row[0]
      except:
        print('Invalid run selector "{}"'.format(run_spec))
        raise SystemExit

  def setup_bounds(self):
    SQL = """
      SELECT DISTINCT
        MAX(  base & ~(?2 - 1),                      COALESCE(?3 & ~(?2 - 1), 0x8000000000000000)) AS block_from,
        MIN(((base & ~(?2 - 1)) + size) & ~(?2 - 1), COALESCE(?4 & ~(?2 - 1), 0x7fffffffffffffff)) AS block_to
      FROM allocs
      WHERE run_id = ?1
        AND block_from <= block_to
      UNION
      SELECT DISTINCT
        MAX(addr & ~(?2 - 1), COALESCE(?3 & ~(?2 - 1), 0x8000000000000000)) AS block_from,
        MIN(addr & ~(?2 - 1), COALESCE(?4 & ~(?2 - 1), 0x7fffffffffffffff)) AS block_to
      FROM access
      WHERE run_id = ?1
        AND block_from <= block_to
      ORDER BY block_from ASC, block_to ASC;
    """
    self.blocks = list()
    self.end_block = None
    self.end_vblock = None
    self.end_time = None
    vpos = 0
    cur_from, cur_to = None, None
    for row in self.db.execute(SQL, (self.run_id, self.addr_unit, self.addr_min, self.addr_max)):
      next_from, next_to = row
      if cur_from is None:
        cur_from, cur_to = next_from, next_to
      else:
        if next_from > (cur_to + self.addr_unit):
          self.blocks.append((cur_from, cur_to, vpos))
          vpos += cur_to - cur_from + self.addr_unit
          cur_from, cur_to = next_from, next_to
        elif next_to > cur_to:
          cur_to = next_to
    if cur_from is not None:
      self.blocks.append((cur_from, cur_to, vpos))
      self.end_block = cur_to
      self.end_vblock = vpos + cur_to - cur_from
    SQL = """
      SELECT MAX(at_ns - (at_ns % ?2))
      FROM access
      WHERE run_id = ?1;
    """
    for row in self.db.execute(SQL, (self.run_id, self.time_unit)):
      self.end_time = row[0] + self.time_unit
    # print('\n'.join('{:>16x} -> {:<16x}   ({:d} x {:d})'.format(block[0], block[1], block[2]//self.addr_unit, self.addr_unit) for block in self.blocks))

  def get_vblock(self, block):
    map_begin = 0
    map_end = len(self.blocks)
    # print('Search  @({:d}..{:d}) for [{:16x}]'.format(map_begin, map_end, block))
    while map_begin < map_end:
      pos = (map_begin + map_end) // 2
      cur_from, cur_to, cur_vblock = self.blocks[pos]
      # print('Checking @{:d} [{:>16x}-{:<16x}]'.format(pos, cur_from, cur_to))
      if cur_from <= block <= cur_to:
        # print('Hit')
        return block - cur_from + cur_vblock
      elif block < cur_from:
        map_end = pos
        # print('Below @({:d}..{:d})'.format(map_begin, map_end))
      else: #cur_to < cur_from
        map_begin = pos + 1
        # print('Above @({:d}..{:d})'.format(map_begin, map_end))
    return None

  def get_accesses(self):
    SQL = """
      SELECT
        at_ns - (at_ns % ?5) AS span, addr & ~(?2 - 1) AS block, is_write, COUNT(*) AS accesses
      FROM access
      WHERE run_id = ?1
        AND COALESCE(block >= (?3 & ~(?2 - 1)), TRUE)
        AND COALESCE(block <= ?4, TRUE)
        AND COALESCE(span  >= (?6 - (?6 % ?5)), TRUE)
        AND COALESCE(span  <= ?7, TRUE)
      GROUP BY span, block, is_write
      ORDER BY span ASC, block ASC, is_write ASC;
    """
    last_span = None
    last_block = None
    last_weight = None
    for row in self.db.execute(SQL, (self.run_id, self.addr_unit, self.addr_min, self.addr_max, self.time_unit, self.time_min, self.time_max)):
      span, block, is_write, weight = row
      if is_write:
        if last_span == span and last_block == block:
          total = last_weight + weight
          span_sec = span and span / 1000000000.0
          block_pos = block + self.addr_unit // 2
          vblock_pos = self.get_vblock(block_pos)
          yield (span_sec, block_pos, vblock_pos, total, last_weight / total)
        else:
          if last_span is not None:
            span_sec = last_span / 1000000000.0
            block_pos = last_block + self.addr_unit // 2
            vblock_pos = self.get_vblock(block_pos)
            yield (span_sec, block_pos, vblock_pos, last_weight, 1.0)
          span_sec = span / 1000000000.0
          block_pos = block + self.addr_unit // 2
          vblock_pos = self.get_vblock(block_pos)
          yield (span_sec, block_pos, vblock_pos, weight, 0.0)
        last_span = None
      else:
        if last_span is not None:
          span_sec = last_span / 1000000000.0
          block_pos = last_block + self.addr_unit // 2
          vblock_pos = self.get_vblock(block_pos)
          yield (span_sec, block_pos, vblock_pos, last_weight, 1.0)
        last_span, last_block, last_weight = span, block, weight

  def get_allocs(self):
    SQL = """
      SELECT DISTINCT
        from_ns - (from_ns % ?5) AS span_from,
        to_ns - (to_ns % ?5) + ?5 AS span_to,
        base & ~(?2 - 1) AS block_from,
        ((base & ~(?2 - 1)) + size) & ~(?2 - 1) AS block_to
      FROM allocs
      WHERE run_id = ?1
        AND COALESCE(span_to    >= (?6 - (?6 % ?5)), TRUE)
        AND COALESCE(span_from  <= ?7, TRUE)
        AND COALESCE(block_to   >= (?3 & ~(?2 - 1)), TRUE)
        AND COALESCE(block_from <= ?4, TRUE);
    """
    for row in self.db.execute(SQL, (self.run_id, self.addr_unit, self.addr_min, self.addr_max, self.time_unit, self.time_min, self.time_max)):
      span_from, span_to, block_from, block_to = row
      if block_from is None or block_to is None:
        continue
      span_from_sec = (span_from or 0) / 1000000000.0
      span_to_sec = (span_to or self.end_time) / 1000000000.0
      block_size = block_to - block_from + self.addr_unit
      vblock_pos = self.get_vblock(block_from)
      yield (span_from_sec, span_to_sec, block_from, block_from+block_size, vblock_pos, vblock_pos+block_size)

  def render_accesses(self):
    def on_xlim_changed(ax):
      print("xlim_changed({}, {})".format(*ax.get_xlim()))
    def on_ylim_changed(ax):
      print("ylim_changed({}, {})".format(*ax.get_ylim()))
    data = pd.DataFrame(self.get_accesses(), columns=('timespan', 'block', 'vblock', 'count', 'reads'))
    axes = sns.scatterplot(
        data=data,
        x='timespan',
        y='vblock',
        size='count',
        hue='reads',
        palette=self.colormap,
        alpha=0.75 if self.args.borderless else 1.0,
        edgecolor='none' if self.args.borderless else 'white')
    # import pdb; pdb.set_trace()

    axes.callbacks.connect('xlim_changed', on_xlim_changed)
    axes.callbacks.connect('ylim_changed', on_ylim_changed)
    axes.legend(loc='center left', bbox_to_anchor=(1.0, 0.5), ncol=1)
    axes.get_legend().remove()
    axes.set_xlabel('Time', size=28, color='#1c345d')
    axes.set_ylabel('Address', size=28, color='#1c345d')
    axes.get_xaxis().set_major_formatter(ticker.EngFormatter(unit='sec'))
    #axes.get_yaxis().set_major_locator(ticker.MultipleLocator(1))
    axes.get_yaxis().set_major_formatter(ticker.FuncFormatter(lambda x, pos: "%x" % int(x)))
    axes.tick_params(axis='both', labelsize=24, colors='#1c345d')
    axes.get_figure().patch.set_alpha(0.0)
    return axes

  def render_allocs(self, axes):
    for span_from, span_to, block_from, block_to, vblock_from, vblock_to in self.get_allocs():
      span_from, span_to = span_from or 0, span_to or self.end_time
      vblock_from, vblock_to = vblock_from or 0, vblock_to or self.end_vblock
      orig = (span_from, vblock_from)
      dx = span_to - span_from
      dy = vblock_to - vblock_from
      rect = patches.Rectangle(orig, dx, dy, linewidth=2, edgecolor=self.allocline, facecolor=self.allocfill, zorder=-1)
      axes.add_patch(rect)

  def render_bounds(self, axes):
    last_to = None
    for block_from, block_to, vblock_from in self.blocks:

      if (self.addr_min is None or block_to >= self.addr_min & ~(self.addr_unit-1)) and (self.addr_max is None or block_to <= self.addr_max & ~(self.addr_unit-1)):
        if last_to is not None:
          orig = ((self.time_min or 0) / 1000000000.0, vblock_from)
          dx = self.end_time / 1000000000.0
          dy = 1
          rect = patches.Rectangle(orig, dx, dy, linewidth=1, edgecolor=self.boundline, facecolor=None, zorder=-1.5)
          axes.add_patch(rect)
        last_to = block_to

  def main(self):
    # logging.basicConfig(level=logging.INFO)

    if self.args.list:
      self.list_run_ids()
    elif self.args.stat:
      self.print_run_stats()
    else:
      sns.set_theme()
      plt.rcParams['font.family'] = 'Source Sans Pro'
      plt.rcParams['savefig.dpi'] = 360
      self.setup_plot()
      # import pdb; pdb.set_trace()
      axes = self.render_accesses()
      self.render_allocs(axes)
      self.render_bounds(axes)
      plt.get_current_fig_manager().set_window_title(self.args.run)
      plt.show()

if __name__ == "__main__":
  heimdallr_viz = HeimdallrViz()
  heimdallr_viz.main()

