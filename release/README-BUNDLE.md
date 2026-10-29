# RyzenHV: готовая сборка

`Hypervisor.framework` на AMD Ryzen (хакинтош, OpenCore). Внутри всё собрано: кекст, шим, QEMU и установщик `rhv`.
Компилировать ничего не нужно, Xcode и Homebrew не требуются.

Проверено: Ryzen 7 5800H, macOS 15.8.1, OpenCore. Parallels Desktop только **26.5.0 (build 57613)**. Это экспериментальный
проект: ставь сначала на тестовый EFI (например, на флешку) и держи рабочий EFI без кекста.

## Установка

```bash
tar xzf ryzenhv-*-macos-x86_64.tar.gz && cd ryzenhv-*/
xattr -dr com.apple.quarantine .      # если архив скачан браузером
./rhv                                 # меню
```

1. **`./rhv kext`** копирует `RyzenHV.kext` в EFI/OC/Kexts, добавляет запись в `config.plist` (с резервной копией и проверкой)
   и дописывает boot-args `rhv_enable_svm=1 rhv_allow_users=1 amfi_get_out_of_my_way=0x1`. Перезагрузись с этого EFI.
   В BIOS должен быть включён SVM Mode.
2. **`./rhv doctor`** проверяет, что всё на месте: CPU, boot-args, кекст (его ABI должен совпадать с этой сборкой), QEMU, Parallels.
3. Дальше нужное тебе:
   - **Docker:** `./rhv docker up`, потом `eval "$(./rhv docker env)"` и `docker run --rm hello-world`.
     Это маленькая Linux-ВМ (Alpine, 2 vCPU, 2 ГиБ) с `dockerd`, образы живут в `~/.ryzenhv/docker/disk.raw`.
     Порт контейнера наружу: `./rhv docker up --port 8080`, затем `docker run -p 8080:80 nginx` и `http://127.0.0.1:8080`. Остановка: `./rhv docker down`.
     Первый запуск скачивает около 150 МБ (ядро Alpine, пакеты, docker CLI) и занимает несколько минут.
   - **Parallels Desktop 26.5.0:** `./rhv parallels`, потом настройки ВМ, которые он напечатает. Отмена: `./rhv parallels undo`.

## Что внутри

| путь | что |
|---|---|
| `rhv` | установщик и запуск |
| `RyzenHV.kext` | кекст (AMD-V / SVM), без подписи, грузится через OpenCore |
| `lib/libryzenhv.dylib` | шим `Hypervisor.framework` |
| `lib/libPrlXmlModel.1.dylib` | подмена библиотеки Parallels (только build 57613) |
| `qemu/` | QEMU 11.1.2 с HVF и user-сетью (GPL, исходники: `LICENSES/SOURCES.txt`) |
| `bin/rhv-info` | диагностика кекста (`rhv-info`, `rhv-info log`) |
| `docs/` | подробности: Parallels, Docker, установка на Ryzentosh |

## Если что-то не так

`./rhv doctor` говорит, что чинить. Для кекста: логи `./bin/rhv-info log`; если система не грузится, выбери в OpenCore
запасной EFI без кекста. Проект: https://github.com/OverlordDev/ryzen-hypervisor.framework
