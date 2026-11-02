#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
work=${WORKDIR:-$root/work/reclaim-qemu}
image=${KERNEL_IMAGE:-/home/lrc/patent/md-fusion-fix-20261031/dist/Image}
mode=${MODE:-manual}
[[ "$mode" == manual || "$mode" == automatic ]]
mkdir -p "$work/root"/{bin,proc,sys,dev,tmp}
cp /home/lrc/patent/kernel/kernel_platform/common/tools/testing/memory_delegation/busybox-1.36.1/busybox "$work/root/bin/busybox"
ln -sfn busybox "$work/root/bin/sh"
flags=(-std=c++17 -O2 -g -pthread -Wall -Wextra -Werror -static -I"$root")
aarch64-linux-gnu-g++ "${flags[@]}" -DMD_TEST_RECLAIM "$root/src/delegation_backend.cpp" "$root/src/kernel_bridge.cpp" "$root/tests/backend_reclaim_test.cpp" -o "$work/root/bin/test"
aarch64-linux-gnu-g++ "${flags[@]}" -DMD_BROKER_MEMFD "$root/src/delegation_backend.cpp" "$root/src/kernel_bridge.cpp" "$root/tools/memory_delegation_backend_broker.cpp" -o "$work/root/bin/broker"
printf '%s\n' "$mode" > "$work/root/mode"
cat > "$work/root/init" <<'INIT'
#!/bin/sh
export PATH=/bin
busybox mount -t proc proc /proc
busybox mount -t sysfs sys /sys
busybox mknod -m 666 /dev/null c 1 3
export MD_KERNEL_BRIDGE=required EXPECT_KERNEL=1 MEMORY_DELEGATION_DOMAIN=reclaim_v9
export MD_BROKER_SOCKET=/tmp/broker.sock
export GLIBC_TUNABLES=glibc.pthread.rseq=0
export MD_RECLAIM=0
if busybox grep -q automatic /mode; then
  export MD_RECLAIM=1 MD_TEST_AUTOMATIC=1 MD_RECLAIM_IDLE_MS=100
fi
fail() { busybox cat /tmp/broker.log; echo RECLAIM_QEMU_FAIL; busybox poweroff -f; exit 1; }
/bin/broker "$MD_BROKER_SOCKET" "$MEMORY_DELEGATION_DOMAIN" >/tmp/broker.log 2>&1 &
i=0
until busybox grep -q BACKEND_BROKER_READY /tmp/broker.log; do
  i=$((i+1)); test "$i" -lt 50 || fail
  busybox sleep 0.1
done
/bin/test || fail
busybox cat /tmp/broker.log
echo RECLAIM_QEMU_PASS
busybox poweroff -f
INIT
chmod +x "$work/root/init"
(cd "$work/root" && find . -print0 | cpio --null -o --format=newc 2>/dev/null | gzip -1) > "$work/initramfs.gz"
timeout 90 qemu-system-aarch64 -machine virt,gic-version=3 -cpu cortex-a57 -smp 4 -m 2048 -nographic -no-reboot -kernel "$image" -initrd "$work/initramfs.gz" -append 'console=ttyAMA0 rdinit=/init panic=1 oops=panic loglevel=4' > "$work/serial.log" 2>&1
if ! grep -q RECLAIM_QEMU_PASS "$work/serial.log" || grep -Eq 'RECLAIM_QEMU_FAIL|Kernel panic|Oops:|BUG:|WARNING:' "$work/serial.log"; then tail -100 "$work/serial.log"; exit 1; fi
grep -E 'PASS|RECLAIM|BROKER_READY' "$work/serial.log"
