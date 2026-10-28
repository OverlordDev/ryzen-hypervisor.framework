# ryzen-hypervisor.framework

Попытка заставить `Hypervisor.framework` работать на **Ryzentosh** (хакинтош на AMD Ryzen).

Нативный `Hypervisor.framework` на Intel-маках построен поверх Intel VT-x (VMX), поэтому на Ryzen
`kern.hv_support` равен 0 и всё, что на него опирается (QEMU/HVF, Docker Desktop, эмуляторы
Android, современные VMware/Parallels), не запускается. Здесь строится слой на **AMD-V (SVM)**,
который предоставляет тот же API.

> **Статус: Windows 11 запускается в Parallels Desktop на Ryzen** (Ryzen 7 5800H, Sequoia 15.8.1, OpenCore, до 4 vCPU,
> установка и работа гостя, клавиатура, сеть). Пройдены фазы 0–2 (SVM на всех ядрах, гость исполняет код, память процесса
> как память гостя, NPT) и фаза 3 (слой совместимости с VMX API: QEMU/HVF грузит Linux, Parallels грузит Windows).
> **Как установить Parallels с этим кекстом: `docs/PARALLELS.md`.** Работает на точной сборке Parallels 26.5.0 (57613).
> Не проверено: сон/пробуждение, `kextunload`, час стабильности, гости кроме Windows 11. 3D-ускорение Parallels на машине
> автора падает в драйвере Metal для NVIDIA, оставляй выключенным.
> Если что-то падает при загрузке, начни с `docs/DEBUGGING.md` (`rhv_stage`, лог OpenCore, `check-kext-symbols.sh`).

Целевое железо: Ryzen 7 5800H (Zen 3, Cezanne, family 19h model 50h), macOS Sequoia (15), OpenCore.

## Структура

```
include/RyzenHVShared.h   ABI между кекстом, шимом и утилитами (селекторы, RHVInfo)
include/RHVRing.h         кольцевой буфер лога (чистый C, тестируется на хосте)
kext/                     RyzenHV.kext: SVM, лог, счётчики, сон/пробуждение, VMCB, VMRUN, NPT (Npt.hpp), IOUserClient
shim/                     userland-слой Hypervisor.framework: hv_shim.c (перехват hv_*), rhv_core.c (семантика VMX API, кэш,
                          несколько vCPU), rhv_iokit.c (связь с кекстом), hijack.c (внедрение в Parallels)
tools/rhv-info/           утилита: инфо, лог, счётчики, самопроверка
tools/rhv-guest-test/     приёмочный тест фазы 1: гость печатает строку, делает CPUID, останавливается
tools/rhv-map-test/       приёмочный тест фазы 2: память гостя = память процесса, protect/unmap, NPF
tools/rhv-vmx-test/       фаза 3: гость ведётся через поля VMCS, как это делает клиент Hypervisor.framework
tools/rhv-long-test/      фаза 3: 64-бит гость с paging, x87/SSE/AVX, прерывания хоста
tools/hvf-smoke/          фаза 3: мини-VMM на ПУБЛИЧНОМ API Hypervisor.framework, работает через шим (scripts/run-with-shim.sh)
scripts/                  collect-debug.sh (отчёт для разбора), install-to-efi.sh, parallels-shim.sh (Parallels), qemu-hvf-test.sh
tests/host_test.c         unit-тесты чистых функций, запускаются где угодно
docs/                     архитектура, roadmap, Ryzentosh, отладка, реверс
```

## Быстрый старт

1. Прочитай `docs/RYZENTOSH.md`: включи SVM в BIOS, сделай **запасную запись OpenCore без кекста**.
2. `make test` : проверить, что чистая логика собирается (работает на любой ОС).
3. `make kext tools` : собрать `build/RyzenHV.kext` и `build/rhv-info`.
4. Положить кекст в `EFI/OC/Kexts`, добавить запись в `Kernel -> Add`, перезагрузиться.
5. `./build/rhv-info` : увидеть CPU, SVM, ASID и фичи (NPT, NRIPS, VmcbClean, ...).
6. Для включения SVM на всех ядрах добавь boot-arg `rhv_enable_svm=1`. По умолчанию кекст только читает.
7. Что-то пошло не так: `./scripts/collect-debug.sh` соберёт всё нужное для разбора в один файл.
   Если система не грузится, добавь boot-arg `-rhv_off` (кекст ничего не делает).

## Диагностика

```
rhv-info                  CPU / SVM
rhv-info log [-f]         кольцевой лог ядра (-f: непрерывно)
rhv-info stats            счётчики событий
sudo rhv-info level 0-5   уровень лога на лету
sudo rhv-info selftest    проверить EFER/VM_HSAVE_PA на каждом ядре
```

Boot-args: `-rhv_off`, `rhv_enable_svm`, `rhv_selftest`, `rhv_loglevel`, `rhv_iolog`, `rhv_stage`, `rhv_slow`. Подробности в `docs/DEBUGGING.md`.

Если на шаге 5 написано `SVM: supported`, `BIOS disable: no`, `NPT: [x]`, значит фаза 0 пройдена
и можно писать VMCB и `VMRUN` (см. `docs/ROADMAP.md`).

## Документация

- `docs/ARCHITECTURE.md`: как устроены слои и почему так
- `docs/ROADMAP.md`: фазы работ и критерии готовности
- `docs/RYZENTOSH.md`: BIOS, OpenCore, как не убить систему
- `docs/DEBUGGING.md`: boot-args, логи, счётчики, самопроверка, что делать при панике
- `docs/PHASE1.md`: как устроен первый гость, как запустить и как читать сбои
- `docs/PHASE2.md`: отображение памяти процесса в гостя, NPF, как запустить и как читать сбои
- `docs/PARALLELS.md`: **установка и использование с Parallels Desktop**, отладка, ограничения
- `docs/PHASE3.md`: слой совместимости с VMX API и ход работ по QEMU и Parallels
- `docs/REVERSING.md`: как достать и изучить родной `Hypervisor.framework`

## Лицензия

Не выбрана. Если будешь переносить код из других проектов: bhyve (FreeBSD) под BSD переносить
можно с сохранением копирайта, Linux KVM под GPL: копировать код нельзя, читать как референс можно.
