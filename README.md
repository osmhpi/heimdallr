# Heimdallr PMU

A library and collection of scripts to capture sparse memory allocation and access traces on a workload.

Heimdallr PMU relies on `perf` and several other features specific to the GNU/Linux platform.
However, successful experiments on Intel x86 and POWER9 indicate compatibility with a variety of hardware platforms.

## Setup

To run the Python scripts, create a fresh `venv` and install the requirements from `vis/requirements.txt`.
```
$ python3 -m venv ./venv
$ source ./venv/bin/activate
$ python3 -m pip install -r vis/requirements.txt
```

To prepare tracing a workload, build the `tracealloc` library, which will interpose the relevant library calls to capture allocation events.
It **requires [libmemkind](https://github.com/memkind/memkind)** for customization of the memory resources used to back workload allocations.
Create a `tracealloc/build/` directory and initialize the environment and build the library.
```
$ mkdir tracealloc/build/ && cd tracealloc/build/
$ cmake ..
$ make -j <ncpu>
```

## Usage

The `runs.sh` script wraps workloads with the necessary infrastructure to collect sparse traces.

```
$ ./runs.sh $HOME/traces_npb 8 $HOME/npb_bin/*.A
```
The example above will run all NPB benchmarks of size class A found in the `$HOME/npb_bin/` directory separately with 8 repetitions.
Results will be written to subdirectories under `$HOME/traces_npb/` for each individual run.

Each argument beginning with the third is taken as the commandline of a separate workload to be executed.

At the top of the `runs.sh` script are default definitions for several variables that should be customized for the target machine:
 * **`HEIMDALLR_READEVENT`** the name of the `perf` event used to sample *read* (load) accesses
 * **`HEIMDALLR_WRITEEVENT`** the name of the `perf` event used to sample *write* (store) accesses
 * **`HEIMDALLR_PMEMDIR`** if PMEM should be used to back allocations, this is the mountpoint for the DAX filesystem
 * **`HEIMDALLR_PMEMSIZE`** if PMEM should be used to back allocations, this is the byte size of the reserved space

At a later point in `runs.sh` follows the definition of execution parameters for multiple series of runs:
```
#     $mode   $hms $mem $cpu $dry $freq
echo "1loc_DR 0    1    1    1        0" >> $modefile
echo "1loc_50 0    1    1    0     5000" >> $modefile
```
Defines two configurations, one dryrun without any instrumentation but `time` to capture the natural runtime of the workload, and one with both the allocation interposer library and `perf record` to capture actual traces, both allocating NUMA local memory.

* **`$mode`** identifier string appended to the result directory to distinguish different configurations
* **`$hms`** when set to `1`, causes the tracealloc library to use PMEM backing memory
* **`$mem`** selects the NUMA node to serve both workload (unless `hms=1`) and instrumentation allocations
* **`$cpu`** selects the NUMA node to pin workload execution to
* **`$dry`** when set to `1`, disables the tracealloc library, i.e., `hms=1` becomes irrelevant and no allocations are captured
* **`$freq`** selects the target sampling frequency for `perf record` in Hz. When set to `0`, disables `perf` instrumentation completely and no accesses are captured

Runs for different configurations are executed in sequence, instead of taking part in the shuffling scheme between workloads and repetitions.

The choice of `$freq` must be supported by the system configuration, as by default the kernel has an upper limit on the sampling frequency which gets lowered dynamically if PMU interrupts take to much time on average.
This behaviour can be changed with the following sysconf parameters:
* `/proc/sys/kernel/perf_event_max_sample_rate` is the upper frequency limit in Hz, which even if explicitly set can be dynamically lowered
* `/proc/sys/kernel/perf_cpu_time_max_percent` modulates the aggressiveness of dynamic limit lowering. If set to `100`, `perf_event_max_sample_rate` will not be modified dynamically, but curiously, no user changes to the value seem to be accepted either

## Visualization

Visualizing trace results is a two-step process: First, raw trace data is parsed from the result directory and reorganized into an sqlite database file, which can hold traces for several runs. Run identifiers are based on result subdirectory names. The actual visualization script queries data from the trace database to improve performance compared to parsing the raw trace files repeatedly on the fly.

```
$ vis/analyze.py -i $HOME/traces_npb -o ./traces_npb.sqlite
```
This command performs the first step, parses and combines the raw result files with access and allocation traces.
In case several iterations of the same run exist in the result directory, only the first repetition is parsed completely, while for subsequent instances only execution statistics are recorded to the database to save space and time.
The `--all` commandline argument overrides this behaviour and parses all repetitions of a run.

The `vis/visualize.py` script works with the resulting trace database:
```
$ vis/visualize.py ./traces_npb.sqlite --list         # (1)
$ vis/visualize.py ./traces_npb.sqlite --stat         # (2)
$ vis/visualize.py ./traces_npb.sqlite --run bt.A.hms # (3)
```
Commandline (1) lists all run identifiers that are present in the trace database, whereas (2) aggregates execution and overhead statistics for each run and its repetitions.
Commandline (3) spawns an interactive matplotlib window visualizing the trace of a given run identifier.
The last form of the command takes several optional parameters to control visual appearance and select trace subsets on the time and address axes for quicker rendering of interesting regions.
Refer to `visualize.py --help` for details.