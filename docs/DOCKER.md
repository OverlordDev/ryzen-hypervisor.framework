# Docker на RyzenHV

Docker Desktop не запускается: ему нужен настоящий `Hypervisor.framework` со своим окружением. Зато работает путь «Linux-гость в QEMU/HVF
через шим, внутри `dockerd`, снаружи обычный `docker` CLI». Проверено на Ryzen 7 5800H, macOS 15.8.1: образы тянутся, `hello-world`,
`alpine` (с выходом в сеть) и `nginx` работают.

## Что нужно

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
