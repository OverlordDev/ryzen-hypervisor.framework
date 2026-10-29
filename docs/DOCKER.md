# Docker on RyzenHV

Docker Desktop does not start: it needs the real `Hypervisor.framework` with its own surroundings. What does work is this route: a Linux guest in QEMU/HVF
through the shim, `dockerd` inside it, and the ordinary `docker` CLI outside. Tested on a Ryzen 7 5800H, macOS 15.8.1: images are pulled, `hello-world`,
`alpine` (with network access) and `nginx` run.

## Ready-made build (the easiest way)

The release (`ryzenhv-<version>-macos-x86_64.tar.gz`) already contains the kext, the shim and QEMU, nothing needs compiling:

```bash
./rhv docker up                      # downloads the Alpine kernel and the docker CLI, installs dockerd in the VM
eval "$(./rhv docker env)"
docker run --rm hello-world
./rhv docker down
```

A container port on the Mac: `./rhv docker up --port 8080`, then `docker run -p 8080:80 nginx` and `http://127.0.0.1:8080`.
Data lives in `~/.ryzenhv/docker` (`RHV_DOCKER_MEM`, `RHV_DOCKER_CPUS`, `RHV_DOCKER_DISK_GB` change the VM's resources).
The rest of this page is only for people who build everything themselves.

## What you need (building from source)

- A loaded `RyzenHV.kext` and a built shim (`make shim`), see `README.md`.
- QEMU with `--enable-hvf` **and `--enable-slirp`** in `~/rhv-qemu` (how to build it: `docs/PHASE3.md`, stage 6). Without slirp there is no `-netdev user` network:
  build libslirp (`meson setup build --prefix=$HOME/rhv-qemu/prefix`) and rebuild QEMU with `--enable-slirp`.
- No root needed (with `rhv_allow_users=1` in the boot-args).

## Running

```bash
./scripts/docker-hvf-test.sh
```

On the first run the script downloads the Alpine 3.20 kernel, initramfs and modloop and a static docker CLI for macOS (into `~/rhv-qemu/docker`),
boots the guest (2 vCPUs, 2 GiB), installs `docker` with `apk`, mounts `disk.raw` (6 GiB, sparse) at `/var/lib/docker`, starts `dockerd`
on guest port 2375 (forwarded to `127.0.0.1:2375` on the host) and runs `hello-world`.

After that:

```bash
export DOCKER_HOST=tcp://127.0.0.1:2375
~/rhv-qemu/docker/docker/docker run --rm alpine uname -a
```

Stop it with `./scripts/docker-hvf-test.sh stop`. Images live on `disk.raw` and survive a restart; the diskless Alpine system itself
installs docker again on every boot (about a minute).

## Limits

- This is a test guest, not a replacement for Docker Desktop: no volume sharing with macOS, no autostart, container ports that should be
  reachable from outside have to be added to `hostfwd` in the script.
- `dockerd` listens on 2375 without TLS; the port is forwarded to `127.0.0.1` only.
- Running together with Parallels has not been tested.
