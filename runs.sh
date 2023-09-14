#!/bin/bash

function do_run() {
  if test -z "$out"; then
    return
  fi

  if test -n "$mem" -o -n "$cpu"; then
    NUMACMD="numactl"
    if test -n "$mem"; then
      NUMACMD="$NUMACMD --membind=$mem"
    fi
    if test -n "$cpu"; then
      NUMACMD="$NUMACMD --cpunodebind=$cpu"
    fi
  fi

  if test -n "$freq" -a "$freq" -gt "0"; then
    PERFCMD="perf record -o $out/access.dat --all-user -F $freq -d -e \"r4003E:ppp\" -e \"r20016:ppp\""
  fi

  # not setting TRAC_STACKLEVELS disables collection of callstack context
  # not setting TRAC_THRESHOLD disables minimum size for traced allocations
  # setting TRAC_PMEMDIR choses a persistent memkind, default is system ram
  # setting TRAC_PMEMSIZE specifies size of the allocated pmem resource
  if test -z "$DRY" -o "$DRY" -le "0"; then
    if test -n "$libtrac"; then
      TRACCMD="env LD_PRELOAD=$libtrac TRAC_LOGPATH=$out TRAC_THRESHOLD=0x1000"
      if test -n "$hms" -a "$hms" -gt "0"; then
        TRACCMD="$TRACCMD TRAC_PMEMDIR=/mnt/pmem/ TRAC_PMEMSIZE=0x100000000"
      fi
    fi
  fi

  CMD="$NUMACMD time -o $out/time.log $PERFCMD $TRACCMD $binary"

  echo "$CMD" > $out/cmd.log
  echo "$CMD"
  $CMD >$out/console.log 2>$out/error.log
}


if test "$#" -lt "3"; then
  echo "Usage: runs.sh <results_dir> <run_count> (<workload>)+"
  exit 1
fi

results_dir=$(realpath -m $1)
if test -z "$results_dir"; then
  echo "Invalid result directory!"
  exit -1
fi
mkdir -p $results_dir

run_count=$2

shift 2
runfile=$results_dir/runs.lst
 > $runfile
for nrun in $(seq $run_count); do
  for nbinary in $(seq $#); do
    echo "$nrun ${!nbinary}" >> $runfile
  done
done

modefile=$results_dir/modes.lst
 > $modefile
#     $mode   $hms $mem $cpu $dry $freq
# echo "1loc_DR 0    1    1    1        0" >> $modefile
# echo "1loc_00 0    1    1    0        0" >> $modefile
# echo "2far_00 0    0    1    0        0" >> $modefile
# echo "3hms_00 1    1    1    0        0" >> $modefile
# echo "1loc_02 0    1    1    0      200" >> $modefile
# echo "2far_02 0    0    1    0      200" >> $modefile
# echo "3hms_02 1    1    1    0      200" >> $modefile
# echo "1loc_10 0    1    1    0     1000" >> $modefile
# echo "2far_10 0    0    1    0     1000" >> $modefile
# echo "3hms_10 1    1    1    0     1000" >> $modefile
echo "1loc_50 0    1    1    0     5000" >> $modefile
# echo "2far_50 0    0    1    0     5000" >> $modefile
# echo "3hms_50 1    1    1    0     5000" >> $modefile

mypath=$(realpath $0)
libtrac=$(dirname $mypath)/tracealloc/build/libtracealloc.so

cat $modefile | while read mode hms mem cpu dry freq; do
  cat $runfile | shuf | while read run binary; do
    binname="$(basename ${binary%% *}) ${binary#* }"
    outdir=$results_dir/${binname//[^0-9a-zA-Z_.]}.$mode.$run
    tmpdir=$(mktemp -d)
    out=$tmpdir
    echo "==============================================================================="
    echo "=== $binary  |  $mode  |  $run"
    echo "==============================================================================="
    do_run
    echo "==============================================================================="
    echo "Finishing..."
    rm -rf $outdir
    mv $tmpdir $outdir
    sync
    sleep 1
    echo
  done
done


