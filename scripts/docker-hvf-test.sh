#!/bin/bash
#
# Brings up Docker on the AMD host: an Alpine Linux guest in QEMU's HVF accelerator (through the RyzenHV shim)
# runs dockerd, and the macOS docker CLI talks to it over 127.0.0.1:2375.
#
#   ./scripts/docker-hvf-test.sh            # download guest files (first run), boot, install docker, run hello-world
#   DOCKER_HOST=tcp://127.0.0.1:2375 ~/rhv-qemu/docker/docker/docker ps      # use it afterwards
#   ./scripts/docker-hvf-test.sh stop       # stop the guest
#
# Needs QEMU built with --enable-hvf and --enable-slirp in ~/rhv-qemu (docs/DOCKER.md). No sudo is needed.
# Files live in ~/rhv-qemu/docker; the guest's /var/lib/docker is the disk.raw there, so images survive a restart,
# while the Alpine system itself is diskless and gets docker installed again on every boot.
set -eu
HERE="$(cd "$(dirname "$0")/.." && pwd)"
W="${RHV_QEMU_DIR:-$HOME/rhv-qemu}"
D="$W/docker"
QEMU="$W/src/build-qemu/qemu-system-x86_64"
LIB="$HERE/build/libryzenhv.dylib"
ALPINE=v3.20
BASE="http://dl-cdn.alpinelinux.org/alpine/$ALPINE"
DOCKER_CLI="${RHV_DOCKER_CLI_VERSION:-29.9.2}"
DRIVE="python3 -I $HERE/scripts/docker-guest-drive.py"

if [ "${1:-}" = stop ]; then pkill -f "qemu-system-x86_64.*docker/disk.raw" || true; exit 0; fi

[ -x "$QEMU" ] || { echo "QEMU is not built at $QEMU" >&2; exit 1; }
"$QEMU" -netdev help | grep -qx user || { echo "this QEMU has no user networking: rebuild with --enable-slirp (docs/DOCKER.md)" >&2; exit 1; }
[ -f "$LIB" ] || { echo "build the shim first: make shim" >&2; exit 1; }
pgrep -f "qemu-system-x86_64.*docker/disk.raw" >/dev/null && { echo "the guest is already running (./scripts/docker-hvf-test.sh stop)" >&2; exit 1; }

mkdir -p "$D"
for f in vmlinuz-virt initramfs-virt modloop-virt; do
    [ -f "$D/$f" ] || curl -fSL -o "$D/$f" "$BASE/releases/x86_64/netboot/$f"
done
if [ ! -x "$D/docker/docker" ]; then
    curl -fSL -o "$D/docker-cli.tgz" "https://download.docker.com/mac/static/stable/x86_64/docker-$DOCKER_CLI.tgz"
    tar xzf "$D/docker-cli.tgz" -C "$D"
fi
[ -f "$D/disk.raw" ] || truncate -s 6G "$D/disk.raw"

rm -f /tmp/rhv-docker-serial.log
nohup env DYLD_INSERT_LIBRARIES="$LIB" "$QEMU" -accel hvf -m 2048 -smp 2 -display none -monitor none -no-reboot \
    -serial tcp:127.0.0.1:4555,server=on,wait=off \
    -netdev user,id=n0,hostfwd=tcp:127.0.0.1:2375-:2375,hostfwd=tcp:127.0.0.1:2222-:22 -device virtio-net-pci,netdev=n0 \
    -drive file="$D/disk.raw",if=virtio,format=raw -device virtio-rng-pci \
    -kernel "$D/vmlinuz-virt" -initrd "$D/initramfs-virt" \
    -append "console=ttyS0 ip=dhcp alpine_repo=$BASE/main,$BASE/community modloop=$BASE/releases/x86_64/netboot/modloop-virt" \
    > /tmp/rhv-docker-qemu.out 2>&1 &
echo "guest started (pid $!), waiting for the login prompt..."

$DRIVE login
$DRIVE run 300 "apk add docker e2fsprogs >/dev/null 2>&1; echo apk=\$?" \
    "modprobe ext4; modprobe overlay; modprobe br_netfilter; modprobe veth; rc-service cgroups start >/dev/null 2>&1" \
    "blkid /dev/vda >/dev/null || mkfs.ext4 -q /dev/vda; mkdir -p /var/lib/docker; mount -t ext4 /dev/vda /var/lib/docker" \
    "(nohup dockerd -H unix:///var/run/docker.sock -H tcp://0.0.0.0:2375 >/var/log/dockerd.out 2>&1 &); sleep 25; docker info --format 'dockerd {{.ServerVersion}} {{.Driver}} cpus={{.NCPU}}'"

export DOCKER_HOST=tcp://127.0.0.1:2375
"$D/docker/docker" run --rm hello-world | grep -m1 "Hello from Docker"
echo "docker works: DOCKER_HOST=$DOCKER_HOST $D/docker/docker ps"
