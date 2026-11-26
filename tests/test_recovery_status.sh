#!/bin/sh
# SPDX-License-Identifier: Unlicense
# Run the actual status and validation-failure paths with no block-device I/O.
set -eu
source_root=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir "$work/run"
cat > "$work/lib.sh" <<EOF
OP_RUN='$work/run'
OP_LIB='$source_root/../../overpressure/runtime/usr/libexec/overpressure'
log() { printf '[1.00] %s\n' "\$*" >> "\$OP_RUN/boot.log"; }
head() { printf ''; }
ext_label() {
    case "\$1" in */super) echo pico4-system ;; */userdata) echo pico4-data ;; esac
}
EOF
sed "s|^TASK_RUN=/run/overpressure$|TASK_RUN='$work/run'|" \
    "$source_root/initramfs/usr/libexec/pico4/task-state" > "$work/task-state"
awk -v helper="$work/lib.sh" -v task="$work/task-state" '
$0 == ". /usr/libexec/overpressure/lib.sh" { print ". \"" helper "\""; next }
$0 == ". /usr/libexec/pico4/task-state" { print ". \"" task "\""; next }
{ print }
' "$source_root/initramfs/usr/bin/pico4-install" > "$work/install"
printf 'existing task must survive\n' > "$work/run/task.json"
before=$(sha256sum "$work/run/task.json")
result=$(busybox ash "$work/install" status)
expected=$(printf 'bootmode: normal\nsystem: pico4-system\ndata: pico4-data')
[ "$result" = "$expected" ]
[ "$(sha256sum "$work/run/task.json")" = "$before" ]
[ ! -e "$work/run/install.lock" ]
[ ! -e "$work/run/task.log" ]
[ ! -d "$work/run/task-state.d" ]
[ ! -d "$work/run/install" ]
if busybox ash "$work/install" status unexpected; then exit 1; else [ "$?" = 2 ]; fi
echo "RECOVERY_STATUS_QUERY_OK output preserved; no task/lock/block writes"

if result=$(busybox ash "$work/install" hash ../invalid 1 2>&1); then exit 1; else [ "$?" = 1 ]; fi
failure='hash needs a by-name partition, not a path'
[ "$result" = "pico4-install: $failure" ]
[ "$(cat "$work/run/boot.log")" = "[1.00] install: $failure" ]
expected=$(printf '[running] Backend command running\n[error] %s' "$failure")
[ "$(cat "$work/run/task.log")" = "$expected" ]
grep -qF "\"status\":\"error\",\"detail\":\"$failure\",\"error\":\"$failure\"" "$work/run/task.json"
grep -qF '"locked":false,"readonly_restored":true' "$work/run/task.json"
echo "RECOVERY_INSTALL_LOG_OK English stages; raw diagnostics and failure state preserved"
