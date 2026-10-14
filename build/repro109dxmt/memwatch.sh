#!/bin/bash
# Resource sampler for the DXMT cloud job (REPRO-110 diagnosis of "runner lost communication").
# Prints one compact block to stdout (the live job log) and appends it to the file given as $3,
# every $2 seconds, until it is killed. It only reads: it never creates or touches the work tree.
#   $1  orchestration root (read only; may not exist yet)
#   $2  seconds between samples (default 60)
#   $3  log file (default $RUNNER_TEMP/memwatch.log)
work="${1:?orchestration root}"
every="${2:-60}"
log="${3:-${RUNNER_TEMP:-/tmp}/memwatch.log}"
reports="$work/dxmt-work/reports"
while :; do
  {
    printf '== memwatch %s load=%s pressure=%s swap=[%s]\n' \
      "$(date -u +%H:%M:%S)" "$(sysctl -n vm.loadavg | tr -d '{}' | xargs)" \
      "$(sysctl -n kern.memorystatus_vm_pressure_level 2>/dev/null || echo ?)" \
      "$(sysctl -n vm.swapusage | tr -s ' ')"
    vm_stat | grep -E 'page size|Pages (free|active|inactive|wired down|occupied by compressor)|Pageouts|Swapouts' \
      | sed -E 's/Mach Virtual Memory Statistics: //; s/ +/ /g' | tr '\n' ';'
    printf '\n'
    ps -Ao rss=,pcpu=,etime=,comm= -m | head -4 | awk '{printf "  rss_mb=%d cpu=%s up=%s %s\n", $1/1024, $2, $3, $4}'
    if [ -d "$reports" ]; then
      newest="$(ls -t "$reports" 2>/dev/null | head -1)"
      if [ -n "$newest" ]; then
        printf '  newest report: %s (%s bytes); tail: ' "$newest" "$(stat -f %z "$reports/$newest")"
        tail -c 160 "$reports/$newest" | tr '\n\r' '  '
        printf '\n'
      fi
    fi
  } 2>&1 | tee -a "$log"
  sleep "$every"
done
