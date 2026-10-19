# Ryzentosh: подготовка, загрузка, отладка

Целевая машина: Ryzen 7 5800H, macOS Sequoia, OpenCore.

## 0. Страховка (сделай до всего остального)

Баг в кекстах виртуализации может уронить ядро при загрузке.

- Сохрани рабочую копию `EFI/` на флешку.
- Держи в OpenCore **вторую запись** (второй `config.plist` или профиль) **без** RyzenHV.kext.
- Не включай `rhv_enable_svm=1` сразу. Сначала только проба.

## 1. BIOS

Включи SVM (на ноутбуках часто называется `SVM Mode`, может быть спрятан в расширенных меню).
Если пункта нет, `rhv-info` покажет `BIOS disable: YES`, и из кекста это не исправить.

## 2. Сборка

```
make test           # чистая логика, должна пройти
make kext tools     # на самом хакинтоше, нужен Xcode / Command Line Tools
```

`kext/Makefile` best effort. Если упадёт, пришли ошибку или собери кекст через Xcode-проект.

## 3. OpenCore: Kernel → Add

Скопируй `build/RyzenHV.kext` в `EFI/OC/Kexts/`. Добавь в `config.plist` запись:

| поле | значение |
|---|---|
| Arch | Any |
| BundlePath | RyzenHV.kext |
| Comment | RyzenHV |
| Enabled | true |
| ExecutablePath | Contents/MacOS/RyzenHV |
| MinKernel / MaxKernel | пусто |
| PlistPath | Contents/Info.plist |

Порядок: после Lilu и остальных ядерных кекстов. Зависимостей от Lilu сейчас нет.

## 4. Проверка

```
./build/rhv-info
./build/rhv-info log
```

В логе ожидай строки вида `svm=1 disabledByBios=0 ...`, затем `probe only; add boot-arg rhv_enable_svm=1 ...`.
Всё про логи, счётчики и самопроверку в `docs/DEBUGGING.md`. Если что-то не так, `./scripts/collect-debug.sh`
соберёт отчёт для разбора.

Что читать из вывода:

| вывод | значит |
|---|---|
| `SVM: NOT supported` | процессор/прошивка не показывает SVM; проверь, что в CPUID не маскируется |
| `BIOS disable: YES` | включить SVM в BIOS |
| `NPT: [ ]` | без nested paging проект не имеет смысла (на Zen 3 такого быть не должно) |
| `usable N < CPUs` | асимметричная конфигурация; кекст откажется включать SVM |

## 5. Включение SVM

Добавь boot-args `rhv_enable_svm=1 rhv_selftest=1` (`Misc → Boot → boot-args`). В логе должно быть
`SVM enabled on N CPUs` и `selftest: N of N CPUs ok`; то же даёт `sudo ./build/rhv-info selftest`.
Если система стабильно работает, фаза 0 пройдена. Если после включения что-то ломается, добавь `-rhv_off`.

## 6. Особенности Ryzentosh

- **Подделка CPUID.** Патчи AMD Vanilla заставляют ядро считать, что стоит Intel. Мы читаем `cpuid` напрямую
  в кексте, поэтому видим настоящий AMD. Тем не менее проверь по `rhv-info`, что `CPU: AMD family 0x19 model 0x50`.
- **Сон/пробуждение.** Состояние SVM после S3 теряется, поэтому кекст выключает SVM перед сном и включает после.
  Эта логика не проверена на железе: после сна смотри `rhv-info stats` и `sudo rhv-info selftest`.
- **Выгрузка.** `kextunload` вызывает `disableAll()`, но с OpenCore-инжектом выгрузка обычно недоступна. Для сброса просто перезагрузись.
