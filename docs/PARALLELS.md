# Parallels Desktop на Ryzen: установка и использование

Состояние: **Windows 11 (Home) устанавливается и работает в Parallels Desktop 26.5.0 (57613) на Ryzen 7 5800H,
macOS Sequoia 15.8.1, OpenCore, до 4 vCPU.** Загрузка до окна установки ~45–50 с, клавиатура, сеть, Parallels Tools
(`prlctl exec`) работают. Всё сделано без правки файлов внутри `/Applications/Parallels Desktop.app`, кроме одной
подмены `prl_vm_app` на обёртку (откатывается).

> Это эксперимент, а не продукт. Работает на **точной сборке Parallels 26.5.0 (57613)**: часть правок привязана к её
> адресам и при другой сборке молча не применяется (ВМ тогда просто не стартует). После обновления Parallels
> повтори шаги 4–5. Правка проприетарного приложения на своей машине это твоя ответственность; в репозитории нет
> ничего от Parallels, копия собирается из установленного у тебя приложения.

## Что нужно

| | |
|---|---|
| Процессор | AMD Ryzen с SVM и NPT (проверено: Ryzen 7 5800H, Zen 3). SVM включён в BIOS |
| Система | macOS Sequoia, OpenCore (проверено 15.8.1), Xcode/Command Line Tools для сборки |
| Parallels Desktop | **26.5.0 (57613)**, пробная версия подходит |
| Гость | проверено: Windows 11 Home (1–4 vCPU). Linux и macOS в роли гостя не проверялись |
| Запасной EFI | флешка с рабочей копией `EFI/` **без** кекста (см. `docs/RYZENTOSH.md`) |

## Установка

1. **Сборка** (на самом хакинтоше):
   ```
   make test            # чистая логика, должна пройти целиком
   make kext tools shim
   ```
2. **Кекст в OpenCore.** Подмонтируй EFI, `./scripts/install-to-efi.sh /Volumes/EFI`, при первой установке добавь запись
   `Kernel → Add` (таблица в `docs/RYZENTOSH.md`).
3. **boot-args** (`Misc → Boot → boot-args`) и перезагрузка:
   ```
   rhv_enable_svm=1 rhv_allow_users=1 amfi_get_out_of_my_way=0x1
   ```
   - `rhv_enable_svm=1`: включить SVM на всех ядрах;
   - `rhv_allow_users=1`: ВМ Parallels работает от обычного пользователя, а не от root;
   - AMFI ослаблен, чтобы запустился процесс с нашей подменной библиотекой (на машине автора ещё стоит `amfi=0x80`).
   Проверка после загрузки: `./build/rhv-info` должен написать `RyzenHV kext (ABI 11)` и `SVM usable on N, active on N`.
4. **Копия Parallels с шимом** (без sudo, лежит в `~/.ryzenhv/prl`):
   ```
   ./scripts/parallels-shim.sh prepare
   ```
5. **Обёртка вместо `prl_vm_app`** (один раз и после каждого обновления Parallels):
   ```
   sudo ./scripts/parallels-shim.sh install
   ./scripts/parallels-shim.sh status
   ```
6. **ВМ.** Создай ВМ в Parallels как обычно (Windows 11 из ISO), затем:
   ```
   prlctl set "Windows 11" --cpus 4                      # проверено 1 и 4 (в кексте до 16 vCPU на ВМ)
   prlctl set "Windows 11" --3d-accelerate off --high-resolution off --high-resolution-in-guest off --vertical-sync off
   ```
   Тип гипервизора должен быть Apple (`prlctl list -i "Windows 11" | grep Hypervisor` → `apple`; иначе
   `prlctl set "Windows 11" --hypervisor-type apple`).
7. Запусти ВМ. На экране «Press any key to boot from CD or DVD» нажми клавишу.

Удаление: `sudo ./scripts/parallels-shim.sh uninstall`, затем убери кекст из `Kernel → Add` и из `EFI/OC/Kexts`.

## Советы по гостю

- **Не убивай ВМ** (`prlctl stop --kill`, закрытие окна с «Stop») во время установки и первого запуска (OOBE): после
  нескольких внеплановых перезапусков Windows показывает «Why did my PC restart?» и уходит в автовосстановление.
- Выключай Windows **полностью**: `shutdown /s /full /t 0`. «Быстрый запуск» хранит старое число процессоров.
- Windows 11 **Home** запускает один физический процессор. Шим сам отдаёт гостю правильную топологию (4 ядра в одном
  пакете, кэш), поэтому все vCPU работают.
- `prlctl exec "Windows 11" cmd /c ...` работает, когда стоят Parallels Tools (можно читать журнал событий, реестр).
- 3D-ускорение (`--3d-accelerate`) на машине автора роняет `prl_vm_app` внутри Metal-драйвера NVIDIA (NVMTL поверх NVK
  для RTX 3070, не штатный драйвер Apple): держи его выключенным. С другим GPU результат может быть другим.

## Что делает шим внутри Parallels

| проблема | решение |
|---|---|
| `kern.hv_support` = 0, Parallels уходит на свой движок | `sysctlbyname` отдаёт 1, `hv_*` подменены на вызовы кекста |
| `prl_vm_app` hardened, `DYLD_INSERT_LIBRARIES` не работает, подпись проверяет диспетчер | зеркало приложения в `~/.ryzenhv/prl`, в нём `libPrlXmlModel.1.dylib` = шим, перезапись импортов по имени (`shim/hijack.c`) |
| проверка вендора `GenuineIntel` (инструкция CPUID) | одна правка перехода в памяти `prl_vm_app` и в копии `libMonitor.dylib` |
| клавиатура не работает | монитор Parallels отбрасывает клавиши, пока гость не запишет 0 в порт 0x80: шим сам вызывает его обработчик |
| triple fault при старте ядра Windows | гипервызов Hyper-V `VMCALL` на AMD это #UD: шим перехватывает #UD и превращает его в обычный VMX-выход |
| гость «спит» после загрузки ядра | страница APIC (0xFEE00000) оставлена без отображения, обращения идут в монитор; гипервизор скрыт от гостя |
| ~60% выходов приходится на CR8 | TPR shadow в ядре (запись CR8 не выходит, пока нет порога), `HV_X86_TPR` 8-битный |
| QPC отрицательный, OOBE «перезапускается» | `RDTSC exiting` перехватывает и `RDTSCP` |
| Windows Home запускает 1 CPU | CPUID: `0x80000008`, `0x80000001`, `0x80000005/6`, лист 4 (ядер в пакете, кэш) |
| несколько vCPU | VM = общая часть (NPT, память) + объект на каждый vCPU; шим открывает по соединению на vCPU |

Подробности и история отладки: `docs/PHASE3.md` (этап 8).

## Отладка

Трасса и статистика **выключены**: они дают сотни тысяч строк в секунду и заметно нагружают систему. Включить:

```
touch /tmp/rhv-debug          # (или ~/.ryzenhv/debug) и перезапустить ВМ
# файлы: /tmp/rhv-prl-<pid>.trace и .stats
rm /tmp/rhv-debug
```

Прочие переключатели (файлы в `/tmp`, действуют при следующем запуске ВМ):

| файл / переменная | эффект |
|---|---|
| `/tmp/rhv-no-vtpr` | не предлагать TPR shadow (вернуть выходы CR8) |
| `/tmp/rhv-show-hv`, `RYZENHV_SHOW_HYPERVISOR=1` | не скрывать гипервизор от гостя (и не править CPUID) |
| `/tmp/rhv-map-apic` | отобразить страницу APIC как память |
| `RYZENHV_NO_KBD_ARM=1` | не «взводить» клавиатуру монитора |
| `/tmp/rhv-dump`, `/tmp/rhv-madt` | (с трассой) дамп регистров/кода гостя раз в секунду; таблица MADT |

С трассой при запросе сброса гостя ищется заголовок аварийного дампа Windows (`BUGCHECK`), при triple fault
печатается состояние гостя (`TRIPLE FAULT`). Падения `prl_vm_app` смотри в `/Library/Logs/DiagnosticReports`
(`prl_vm_app-*.ips`), лог ВМ: `~/Parallels/<ВМ>.pvm/parallels.log`.

## Известные ограничения

- Только сборка Parallels 26.5.0 (57613); после обновления запускай `prepare` и `install` заново, а на другой
  сборке жди, что правки по адресам не применятся (шим пишет об этом в трассу и ничего не трогает).
- Не проверено: сон/пробуждение, `kextunload`, длительная нагрузка, гости кроме Windows 11, snapshot/suspend
  с несколькими vCPU, адресные пространства `hv_vm_space_*`.
- Окно «Why did my PC restart?» после внеплановых остановок: см. советы выше.
- `rhv_allow_users=1` даёт любому локальному пользователю доступ к гипервизору: включай только на личной машине.
