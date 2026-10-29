# Docker на RyzenHV

Docker Desktop не запускается: ему нужен настоящий `Hypervisor.framework` со своим окружением. Зато работает путь «Linux-гость в QEMU/HVF
через шим, внутри `dockerd`, снаружи обычный `docker` CLI». Проверено на Ryzen 7 5800H, macOS 15.8.1: образы тянутся, `hello-world`,
`alpine` (с выходом в сеть) и `nginx` работают.

## Готовая сборка (самый простой путь)

В релизе (`ryzenhv-<версия>-macos-x86_64.tar.gz`) уже лежат кекст, шим и QEMU, ничего компилировать не нужно:

```bash
./rhv docker up                      # скачивает ядро Alpine и docker CLI, ставит dockerd в ВМ
eval "$(./rhv docker env)"
docker run --rm hello-world
./rhv docker down
```

Порт контейнера на Mac: `./rhv docker up --port 8080`, затем `docker run -p 8080:80 nginx` и `http://127.0.0.1:8080`.
Данные живут в `~/.ryzenhv/docker` (`RHV_DOCKER_MEM`, `RHV_DOCKER_CPUS`, `RHV_DOCKER_DISK_GB` меняют ресурсы ВМ).
Остальное ниже нужно только тем, кто собирает всё сам.

## Что нужно (сборка из исходников)

- Загруженный `RyzenHV.kext` и собранный шим (`make shim`), см. `README.md`.
- QEMU с `--enable-hvf` **и `--enable-slirp`** в `~/rhv-qemu` (как его собрать, см. `docs/PHASE3.md`, этап 6). Без slirp нет сети `-netdev user`:
  соберите libslirp (`meson setup build --prefix=$HOME/rhv-qemu/prefix`) и пересоберите QEMU с `--enable-slirp`.
- Root не нужен (при `rhv_allow_users=1` в boot-args).

## Запуск

```bash
./scripts/docker-hvf-test.sh
```

Скрипт при первом запуске скачивает ядро, initramfs и modloop Alpine 3.20 и статический docker CLI для macOS (в `~/rhv-qemu/docker`),
загружает гостя (2 vCPU, 2 ГиБ), ставит `docker` через `apk`, монтирует `disk.raw` (6 ГиБ, sparse) в `/var/lib/docker`, запускает `dockerd`
на порту 2375 гостя (проброшен на `127.0.0.1:2375` хоста) и запускает `hello-world`.

Дальше:

```bash
export DOCKER_HOST=tcp://127.0.0.1:2375
~/rhv-qemu/docker/docker/docker run --rm alpine uname -a
```

Остановка: `./scripts/docker-hvf-test.sh stop`. Образы лежат на `disk.raw` и переживают перезапуск, сама система Alpine без диска
и ставит docker заново при каждой загрузке (около минуты).

## Ограничения

- Это тестовый гость, а не замена Docker Desktop: нет проброса томов с macOS, нет автозапуска, порты контейнеров наружу нужно
  добавлять в `hostfwd` в скрипте.
- `dockerd` слушает 2375 без TLS, наружу порт проброшен только на `127.0.0.1`.
- Одновременная работа с Parallels не проверялась.
