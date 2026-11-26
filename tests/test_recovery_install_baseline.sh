#!/bin/sh
# SPDX-License-Identifier: Unlicense
# Optionally pass a copy of the upstream installer as argument 1.
# Use only temporary files. No device opens, mounts, clock changes or downloads.
set -eu
for tool in busybox zstd flock curl; do
	command -v "$tool" >/dev/null 2>&1 || { echo "SKIP Recovery installer comparison: missing $tool"; exit 0; }
done
source_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

instrument() {
	sed -e 's|^\. /usr/libexec/overpressure/lib.sh$|. "$FIXTURE_LIB"|' \
		-e 's|^\. /usr/libexec/pico4/task-state$|. "$FIXTURE_TASK"|' \
		-e 's|/dev/block/by-name|$FIXTURE/part|g' \
		-e 's|\[ -b "$dev" \]|[ -f "$dev" ]|g' \
		-e 's|/sys/class/block/${1##\*/}/ro|$FIXTURE/ro/${1##*/}|' \
		-e 's|/proc/mounts|"$FIXTURE/mounts"|' \
		-e 's|/proc/sys/vm/drop_caches|"$FIXTURE/drop_caches"|'
}
if [ "$#" -eq 0 ]; then
	if ! git -C "$source_root" show 50113b7d81a421d238fcc5028e9e0da1b8c600e5:initramfs/usr/bin/pico4-install \
		> "$work/baseline" 2>/dev/null; then
		echo 'SKIP Recovery installer comparison: upstream revision unavailable; pass its installer as argument 1'
		exit 0
	fi
	set -- "$work/baseline"
fi
instrument < "$1" > "$work/upstream"
[ -s "$work/upstream" ]
instrument < "$source_root/initramfs/usr/bin/pico4-install" > "$work/local"
sed -e 's|^TASK_RUN=/run/overpressure$|TASK_RUN=$FIXTURE/run|' \
	-e 's|sh /usr/bin/pico4-install|busybox ash "$FIXTURE_INSTALL"|' \
	"$source_root/initramfs/usr/libexec/pico4/task-state" > "$work/task"
export FIXTURE_LIB="$work/lib" FIXTURE_TASK="$work/task" FIXTURE_INSTALL="$work/local"

cat > "$FIXTURE_LIB" <<'EOF'
OP_RUN=$FIXTURE/run
log() { :; }
ext_label() {
	case "$1" in
	*/userdata) [ ! -f "$FIXTURE/formatted" ] || echo pico4-data ;;
	*/super) [ ! -s "$1" ] || echo pico4-system ;;
	esac
}
record() { printf '%s\n' "$*" >> "$FIXTURE/operations"; }
blockdev() {
	[ "$2" = "$FIXTURE/part/${2##*/}" ] || return 9
	case "$1" in
	--getsize64) echo 16384 ;;
	--getro) cat "$FIXTURE/ro/${2##*/}" ;;
	--setrw) record "$@"; echo 0 > "$FIXTURE/ro/${2##*/}" ;;
	--setro) record "$@"; echo 1 > "$FIXTURE/ro/${2##*/}" ;;
	*) return 9 ;;
	esac
}
mount() {
	[ "$#" = 6 ] && [ "$1 $2 $3" = '-t ext4 -o' ] && [ "$5" = "$FIXTURE/part/userdata" ] || return 9
	case "$4" in noatime) rw=rw ;; ro,noload) rw=ro ;; *) return 9 ;; esac
	record "mount $*"
	mv "$6" "$6.unmounted" && ln -s "$FIXTURE/userdata" "$6" &&
		printf '%s %s ext4 %s 0 0\n' "$5" "$6" "$rw,$4" > "$FIXTURE/mounts"
}
umount() {
	[ "$#" = 1 ] && [ "$(readlink "$1")" = "$FIXTURE/userdata" ] || return 9
	record "umount $*"
	rm "$1" && mv "$1.unmounted" "$1" && : > "$FIXTURE/mounts"
}
date() { record "date $*"; }
chown() { record "chown $*"; }
sync() { record "sync $*"; }
dd() { record "dd $*"; busybox dd "$@"; }
tar() {
	if [ "${1:-}" = --version ]; then
		printf 'tar (GNU tar)\n'
	else
		command tar "$@"
	fi
}
sleep() { if [ "$1" = 30 ]; then exit 0; else command sleep "$@"; fi; }
EOF
mkdir "$work/bin"
cat > "$work/bin/mkfs.ext4" <<'EOF'
#!/bin/sh
. "$FIXTURE_LIB"
[ -z "$(ls -A "$FIXTURE/userdata")" ] || exit 9
record "mkfs.ext4 $*"
: > "$FIXTURE/formatted"
EOF
chmod 700 "$work/bin/mkfs.ext4"
export PATH="$work/bin:$PATH"

mkdir -p "$work/source/devices/phoenix/scripts" "$work/stock/assets"
cat > "$work/source/devices/phoenix/scripts/build-on-headset.sh" <<'EOF'
set -eu
[ "$SOURCE_DATE_EPOCH:$OVERPRESSURE_COMMIT:$ALL_PROXY" = '1700000000:fixture:socks5h://127.0.0.1:1234' ]
mkdir -p "$1/root/etc" "$1/out" "${1%/var/tmp/pico4-build}/var/mnt/vendor"
printf 'build-root mode witness\n' > "$1/root/etc/fixture.conf"
printf '{}\n' > "$1/out/pico4-system.img.json"
printf '{}\n' > "$1/out/pico4-userspace.manifest.json"
zstd -q "$FIXTURE/../super" -o "$1/out/pico4-system.img.zst"
EOF
tar -czf "$work/source.tgz" -C "$work/source" devices
printf 'factory fixture\n' > "$work/stock/assets/sample"
printf '{}\n' > "$work/stock/manifest.json"
(cd "$work/stock" && sha256sum assets/sample > SHA256SUMS)
tar -czf "$work/stock.tgz" -C "$work/stock" assets manifest.json SHA256SUMS
printf '\000boot\377\000' > "$work/boot"
printf '\000super\377\000' > "$work/super"
printf 'ssh-ed25519 Zml4dHVyZQ== public-fixture\n' > "$work/keys"
token=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb

call() {
	if [ "$implementation" = broker ]; then
		busybox ash "$FIXTURE_TASK" run "$token" "$@"
	else
		busybox ash "$work/$implementation" "$@"
	fi
}

stage() {
	if [ "$implementation" = broker ]; then
		busybox ash "$FIXTURE_TASK" stage "$token" "$1" running '' >/dev/null
	fi
}

write_image() {
	name=$1
	shift
	bytes=$(wc -c < "$work/$name")
	set -- write "$name" --size 16384 --bytes "$bytes" --sha256 "$(sha256sum "$work/$name" | cut -d' ' -f1)" "$@"
	case "${*:--}" in
	*--from-data*) call "$@" < /dev/null ;;
	*) call "$@" < "$work/$name" ;;
	esac
}

run_install() (
	implementation=$1 route=$2 mask=$3
	export FIXTURE="$work/$mask-$route-$implementation"
	umask "$mask"
	mkdir -p "$FIXTURE/run" "$FIXTURE/part" "$FIXTURE/ro" "$FIXTURE/userdata"
	for part in boot super userdata misc; do
		: > "$FIXTURE/part/$part"
		echo 1 > "$FIXTURE/ro/$part"
	done
	printf boot-recovery > "$FIXTURE/part/misc"
	: > "$FIXTURE/operations"
	: > "$FIXTURE/mounts"
	if [ "$implementation" = broker ]; then
		mkfifo "$FIXTURE/host"
		busybox ash "$FIXTURE_TASK" hold "$token" "$route" < "$FIXTURE/host" > "$FIXTURE/hold" &
		holder=$!
		trap 'exec 8>&-; kill "$holder" 2>/dev/null || :; wait "$holder" 2>/dev/null || :' EXIT
		exec 8> "$FIXTURE/host"
		i=0
		until grep -q "^locked $token$" "$FIXTURE/hold"; do
			[ "$i" -lt 100 ] && kill -0 "$holder"
			i=$((i + 1))
			sleep .02
		done
	fi
	{
		offset=0
		call status
		if [ "$route" = headset ]; then
			stage 1
			call format-data --size 16384
			stage 2
			call build --size 16384 --epoch 1700000000 --commit fixture --proxy socks5h://127.0.0.1:1234 \
				--now 1700000000 < "$work/source.tgz"
			call build-file --size 16384 out/pico4-system.img.json
			call build-file --size 16384 out/pico4-userspace.manifest.json
			offset=2
		fi
		stage $((offset + 1))
		write_image boot
		stage $((offset + 2))
		if [ "$route" = headset ]; then
			write_image super --zstd --from-data var/tmp/pico4-build/out/pico4-system.img.zst
		else
			write_image super
		fi
		call status
		stage $((offset + 3))
		if [ "$route" = host ]; then
			call format-data --size 16384
		fi
		stage $((offset + 4))
		call keys --size 16384 --id fixture < "$work/keys"
		stage $((offset + 5))
		stock_size=$(wc -c < "$work/stock.tgz")
		stock_sha=$(sha256sum "$work/stock.tgz" | cut -d' ' -f1)
		call stock-hash --size 16384
		call stock --size 16384 --bytes "$stock_size" --sha256 "$stock_sha" < "$work/stock.tgz"
		call stock-hash --size 16384
		stage $((offset + 6))
		call bootmode normal
		call status
	} > "$FIXTURE/receipts"
	cmp "$FIXTURE/part/boot" "$work/boot"
	cmp "$FIXTURE/part/super" "$work/super"
	[ "$(wc -c < "$FIXTURE/part/misc")" = 2048 ]
	[ "$(cat "$FIXTURE/ro/"*)" = "$(printf '1\n1\n1\n1')" ]
	[ ! -s "$FIXTURE/mounts" ]
	if [ "$implementation" = broker ]; then
		exec 8>&-
		wait "$holder"
		trap - EXIT
	fi
	if [ "$implementation" != upstream ]; then
		[ "$(stat -c %a "$FIXTURE/run/task-state.d")" = 700 ]
		[ "$(stat -c %a "$FIXTURE/run/install.lock" "$FIXTURE/run/task.json" "$FIXTURE/run/task.log")" = \
			"$(printf '600\n600\n600')" ]
	fi
	find "$FIXTURE/userdata" -printf '%P %m %U:%G %y %l\n' | LC_ALL=C sort > "$FIXTURE/modes"
	sed -e '/^dd /d' -e "s|$FIXTURE|<fixture>|g" "$FIXTURE/operations" > "$FIXTURE/policy"
	[ "$(grep -c '^dd .*conv=fsync' "$FIXTURE/operations")" = 3 ]
)

for mask in 022 027; do
	for route in host headset; do
		run_install upstream "$route" "$mask"
		for implementation in local broker; do
			run_install "$implementation" "$route" "$mask"
			for result in modes policy receipts; do
				diff -u "$work/$mask-$route-upstream/$result" "$work/$mask-$route-$implementation/$result"
			done
		done
		echo "RECOVERY_INSTALL_BASELINE_OK $route umask $mask (direct and broker)"
	done
done
