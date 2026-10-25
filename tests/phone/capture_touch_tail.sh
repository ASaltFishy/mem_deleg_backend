#!/system/bin/sh
set -eu
name=${1:?name}; mode=${2:-light}; size=${3:-268435456}; workers=${4:-4}; iterations=${5:-10}; repeats=${6:-3}
case "$name" in *[!a-zA-Z0-9_]*) exit 2;;esac
case "$mode" in none|light|fault) ;;*) exit 2;;esac
root=/data/local/tmp/md-touch-diagnosis
out=$root/$name
T=/sys/kernel/tracing
instance=$T/instances/mdt_$name
group=mdt_$name
registered=
active=
gate=
mkdir -p "$root"
mkdir "$out"
export MD_KERNEL_BRIDGE=required SCUDO_DELEGATION_BACKEND=1 SCUDO_SHARED_ARENA_DISABLE=1
export MEMORY_DELEGATION_DOMAIN=allocator_delegation_v8
boot=$(cat /proc/sys/kernel/random/boot_id)
export MD_BROKER_SOCKET=/data/local/tmp/md_backend_v8_$boot/backend.sock
[ -S "$MD_BROKER_SOCKET" ]
snapshot() {
 echo "boot_id=$(cat /proc/sys/kernel/random/boot_id)"
 cat /proc/meminfo /proc/swaps /proc/vmstat
 cat /sys/block/zram0/mm_stat 2>/dev/null || true
 for field in scaling_cur_freq scaling_min_freq scaling_max_freq scaling_governor; do
  echo "$field=$(cat /sys/devices/system/cpu/cpu0/cpufreq/$field)"
 done
}
cleanup() {
 set +e
 if [ -n "$active" ];then [ -z "$gate" ] || touch "$gate"; wait "$active";fi
 if [ "$mode" != none ];then
  echo 0 > "$instance/tracing_on"
  cat "$instance/trace" > "$out/trace.txt"
  cat "$instance/per_cpu/cpu0/stats" > "$out/trace-stats.txt"
  cat "$T/kprobe_profile" > "$out/kprobe-profile.txt"
  echo 0 > "$instance/events/enable"
  for event in $registered;do echo "-:$event" >> "$T/kprobe_events";done
  rmdir "$instance"
 fi
 snapshot > "$out/after.txt"
}
trap cleanup EXIT
trap 'exit 130' INT TERM
snapshot > "$out/before.txt"
echo "mode=$mode size=$size workers=$workers iterations=$iterations repeats=$repeats" > "$out/config.txt"
probe() {
 key=$1;symbol=$2
 if [ "$key" = fault ];then
  echo "p:$group/${key}_enter $symbol address=%x1:x64 flags=%x2:u64" >> "$T/kprobe_events"
 else
  echo "p:$group/${key}_enter $symbol" >> "$T/kprobe_events"
 fi
 registered="$registered $group/${key}_enter"
 if [ "$key" = fault ];then
  printf 'r128:%s/%s_exit %s ret=$retval:x64\n' "$group" "$key" "$symbol" >> "$T/kprobe_events"
 else
  printf 'r128:%s/%s_exit %s\n' "$group" "$key" "$symbol" >> "$T/kprobe_events"
 fi
 registered="$registered $group/${key}_exit"
}
if [ "$mode" != none ];then
 mkdir "$instance"
 echo 0 > "$instance/tracing_on"
 echo mono > "$instance/trace_clock"
 echo 1 > "$instance/tracing_cpumask"
 if [ "$mode" = fault ];then echo 131072 > "$instance/buffer_size_kb";else echo 65536 > "$instance/buffer_size_kb";fi
 for event in sched/sched_switch irq/irq_handler_entry irq/irq_handler_exit irq/softirq_entry irq/softirq_exit vmscan/mm_vmscan_direct_reclaim_begin vmscan/mm_vmscan_direct_reclaim_end vmscan/mm_vmscan_memcg_reclaim_begin vmscan/mm_vmscan_memcg_reclaim_end compaction/mm_compaction_begin compaction/mm_compaction_end;do
  [ ! -e "$instance/events/$event/enable" ] || echo 1 > "$instance/events/$event/enable"
 done
 probe sync md_sync_task_work
 probe unmap zap_page_range_single
 probe swapin shmem_swapin_folio
 probe swapread swap_read_folio
 if [ "$mode" = fault ];then probe fault handle_mm_fault;fi
fi
r=1
while [ "$r" -le "$repeats" ];do
 pidfile=$out/pids_$r;gate=$out/gate_$r
 echo "TOUCH_CASE_BEGIN repetition=$r" >> "$out/bench.log"
 MD_TOUCH_PID_FILE="$pidfile" MD_TOUCH_GATE="$gate" /data/local/tmp/md-touch-bench.aarch64 \
  --size "$size" --mode process --cpus 0 --workers-per-core "$workers" --path delegated \
  --allow-fallback 0 --touch-pages 1 --iterations "$iterations" --warmup 5 >> "$out/bench.log" 2>&1 &
 active=$!
 n=0
 while [ ! -s "$pidfile" ] || [ "$(wc -l < "$pidfile")" -ne "$workers" ];do
  n=$((n+1));[ "$n" -lt 3000 ] || { echo 'PID handshake timeout';exit 1; }
  kill -0 "$active" || { cat "$out/bench.log";exit 1; }
  sleep .01
 done
 if [ "$mode" != none ];then
  filter=
  for pid in $(cat "$pidfile");do
   if [ -n "$filter" ];then filter="$filter || ";fi
   filter="${filter}common_pid == $pid"
  done
  echo "$filter" > "$instance/events/$group/filter"
  echo 1 > "$instance/events/$group/enable"
  echo 1 > "$instance/tracing_on"
 fi
 touch "$gate"
 wait "$active"
 active=
 if [ "$mode" != none ];then echo 0 > "$instance/tracing_on";fi
 echo "TOUCH_CASE_PASS repetition=$r"
 r=$((r+1))
done
echo "TOUCH_CAPTURE_PASS output=$out"
