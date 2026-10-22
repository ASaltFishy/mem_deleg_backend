#!/system/bin/sh
set -eu
case "${1:-snapshot}" in
  wait)
    count=0
    while [ "$(getprop sys.boot_completed)" != 1 ]; do
      count=$((count + 1))
      [ "$count" -lt 90 ] || { echo 'boot_completed timeout' >&2; exit 1; }
      sleep 2
    done
    ;;
  snapshot)
    echo "boot_id=$(cat /proc/sys/kernel/random/boot_id)"
    echo "boot_completed=$(getprop sys.boot_completed)"
    uname -a
    cat /proc/uptime /proc/loadavg
    for d in /sys/devices/system/cpu/cpu[0-9]*; do
      for field in online cpufreq/scaling_cur_freq cpufreq/scaling_governor; do
        [ ! -r "$d/$field" ] || echo "$d/$field=$(cat "$d/$field")"
      done
    done
    for d in /sys/class/thermal/thermal_zone*; do
      if [ -r "$d/temp" ]; then
        echo "$d type=$(cat "$d/type" 2>/dev/null || true) temp=$(cat "$d/temp")"
      fi
    done
    for p in /proc/pressure/cpu /proc/pressure/memory; do
      [ ! -r "$p" ] || cat "$p"
    done
    ;;
  *) exit 2 ;;
esac
