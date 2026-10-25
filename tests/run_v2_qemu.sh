#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
kplat=${KERNEL_PLATFORM:-/home/lrc/patent/kernel/kernel_platform}
image=${KERNEL_IMAGE:-/home/lrc/patent/md-v2-work/dist/Image}
work=${WORKDIR:-$root/work/v2-qemu}
mkdir -p "$work/root"/{bin,proc,sys,dev,tmp}
cp "$kplat/common/tools/testing/memory_delegation/busybox-1.36.1/busybox" "$work/root/bin/busybox"
ln -sfn busybox "$work/root/bin/sh"
aarch64-linux-gnu-g++ -std=c++17 -O2 -pthread -Wall -Wextra -Werror -static -DMD_TEST_MIGRATION -I"$root" \
  "$root/src/delegation_backend.cpp" "$root/src/kernel_bridge.cpp" "$root/tests/backend_v2_test.cpp" \
  -o "$work/root/bin/backend_v2_test"
aarch64-linux-gnu-g++ -std=c++17 -O2 -pthread -Wall -Wextra -Werror -static -DMD_BROKER_MEMFD -I"$root" \
  "$root/src/delegation_backend.cpp" "$root/src/kernel_bridge.cpp" "$root/tools/memory_delegation_backend_broker.cpp" \
  -o "$work/root/bin/memory_delegation_backend_broker"
aarch64-linux-gnu-g++ -std=c++17 -O2 -pthread -Wall -Wextra -Werror -static -I"$root" \
  "$root/src/delegation_backend.cpp" "$root/src/kernel_bridge.cpp" "$root/tests/cpu_reader_test.cpp" \
  -o "$work/root/bin/cpu_reader_test"
latency=${RUN_LATENCY:-1}
matrix=${MATRIX:-quick}
iterations=${ITERATIONS:-10}; warmup=${WARMUP:-5}; repetitions=${REPETITIONS:-2}
[[ "$matrix" == quick || "$matrix" == full ]]
for value in "$iterations" "$warmup" "$repetitions"; do [[ "$value" =~ ^[1-9][0-9]*$ ]]; done
# Avoid executing stale optional binaries from a previous invocation.
rm -f "$work/root/bin/allocator_latency_jemalloc"
rm -f "$work/root/enable_latency"
rm -f "$work/root/bin/scudo_capacity_test"
if [[ "${RUN_CAPACITY:-0}" == 1 ]]; then
  cp "$root/build/scudo_capacity_test.linux.aarch64" "$work/root/bin/scudo_capacity_test"
fi
if [[ "$latency" == 1 ]]; then
  strings "$root/build/allocator_latency_scudo.linux.aarch64" | grep "bench_protocol=shared_rings_v3" >/dev/null || { echo "Rebuild stale scudo binary for ABI v3" >&2; exit 1; }
  cp "$root/build/allocator_latency_scudo.linux.aarch64" "$work/root/bin/allocator_latency_scudo"
  touch "$work/root/enable_latency"
  if [[ "${RUN_JEMALLOC:-0}" == 1 ]]; then
    strings "$root/build/allocator_latency_jemalloc.linux.aarch64" | grep "bench_protocol=shared_rings_v3" >/dev/null || { echo "Rebuild stale jemalloc binary for ABI v3" >&2; exit 1; }
    cp "$root/build/allocator_latency_jemalloc.linux.aarch64" "$work/root/bin/allocator_latency_jemalloc"
  fi
fi
cat > "$work/root/init" <<'INIT'
#!/bin/sh
export PATH=/bin
busybox mount -t proc proc /proc
busybox mount -t sysfs sys /sys
busybox mknod -m 666 /dev/null c 1 3
busybox mkdir -p /dev/shm
busybox mount -t tmpfs -o size=75% tmpfs /dev/shm
ulimit -c 0
export MD_KERNEL_BRIDGE=required EXPECT_KERNEL=1 MEMORY_DELEGATION_DOMAIN=md_v2_qemu
export SCUDO_SHARED_ARENA_DISABLE=1
fail() { echo "V2_QEMU_FAIL $*"; busybox poweroff -f; exit 1; }
export MD_BROKER_SOCKET=/tmp/md-backend.sock
/bin/memory_delegation_backend_broker "$MD_BROKER_SOCKET" "$MEMORY_DELEGATION_DOMAIN" > /tmp/broker.log 2>&1 &
i=0
until busybox grep -q BACKEND_BROKER_READY /tmp/broker.log; do
  i=$((i+1)); test "$i" -lt 50 || { busybox cat /tmp/broker.log; fail broker; }
  busybox sleep 0.1
done
EXPECT_RSEQ=1 /bin/cpu_reader_test || fail cpu_reader
GLIBC_TUNABLES=glibc.pthread.rseq=0 EXPECT_RSEQ=1 /bin/cpu_reader_test || fail cpu_reader_owned
MD_CPU_QUERY=syscall /bin/cpu_reader_test || fail cpu_reader_fallback
GLIBC_TUNABLES=glibc.pthread.rseq=0 /bin/backend_v2_test || fail backend
if test -x /bin/scudo_capacity_test; then
  SCUDO_DELEGATION_BACKEND=1 GLIBC_TUNABLES=glibc.pthread.rseq=0 /bin/scudo_capacity_test || fail capacity
fi
if test -f /enable_latency; then
  export GLIBC_TUNABLES=glibc.pthread.rseq=0
  . /latency-config
  sizes=131072; modes=thread; shapes=multicore; touches=1
  if test "$MATRIX" = full; then sizes="131072 262144 1048576"; modes="thread process"; shapes="single shared multicore"; touches="0 1"; fi
  echo "V2_CONFIG matrix=$MATRIX warmup=$WARMUP iterations=$ITERATIONS repetitions=$REPETITIONS"
  for allocator in scudo jemalloc; do
    test -x /bin/allocator_latency_$allocator || continue
  for size in $sizes; do
    for mode in $modes; do
      for shape in $shapes; do
        cpus=0; workers=1
        test "$shape" != shared || workers=2
        test "$shape" != multicore || cpus=0,1
        for touch in $touches; do
          for path in traditional delegated; do
            if test "$path" = delegated; then export SCUDO_DELEGATION_BACKEND=1; else export SCUDO_DELEGATION_BACKEND=0; fi
            repetition=1
            while test "$repetition" -le "$REPETITIONS"; do
            echo "V2_CASE_BEGIN allocator=$allocator size=$size mode=$mode shape=$shape touch=$touch path=$path repetition=$repetition"
            /bin/allocator_latency_$allocator --size "$size" --mode "$mode" --cpus "$cpus" --workers-per-core "$workers" \
              --path "$path" --touch-pages "$touch" --iterations "$ITERATIONS" --warmup "$WARMUP" || fail latency
            echo "V2_CASE_PASS allocator=$allocator size=$size mode=$mode shape=$shape touch=$touch path=$path repetition=$repetition"
            repetition=$((repetition+1))
            done
          done
        done
      done
    done
  done
  done
fi
busybox cat /tmp/broker.log
echo V2_QEMU_ALL_PASS
busybox poweroff -f
INIT
# Store validated host settings separately from the literal init script.
printf 'export MATRIX=%s ITERATIONS=%s WARMUP=%s REPETITIONS=%s\n' "$matrix" "$iterations" "$warmup" "$repetitions" > "$work/root/latency-config"
chmod +x "$work/root/init"
(cd "$work/root" && find . -print0 | cpio --null -o --format=newc 2>/dev/null | gzip -1) > "$work/initramfs.gz"
timeout "${TIMEOUT:-300}" qemu-system-aarch64 -machine virt,gic-version=3 -cpu cortex-a57 -smp 4 -m 2048 \
  -nographic -no-reboot -kernel "$image" -initrd "$work/initramfs.gz" \
  -append 'console=ttyAMA0 rdinit=/init panic=1 oops=panic loglevel=4' > "$work/serial.log" 2>&1
if ! grep -q V2_QEMU_ALL_PASS "$work/serial.log" || grep -Eq 'V2_QEMU_FAIL|FAIL\(backend|Kernel panic|Oops:|BUG:|WARNING:' "$work/serial.log"; then
  tail -100 "$work/serial.log"; exit 1
fi
if [[ "$latency" == 1 ]]; then
  groups=2
  [[ "$matrix" != full ]] || groups=72
  engines=1
  [[ ! -x "$work/root/bin/allocator_latency_jemalloc" ]] || engines=2
  expected=$((groups * engines * repetitions))
  [[ $(grep -c V2_CASE_PASS "$work/serial.log") == "$expected" ]]
fi
grep -E 'PASS\(backend|V2_CASE_PASS|V2_QEMU_ALL_PASS' "$work/serial.log"
