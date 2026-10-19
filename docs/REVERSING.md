# Реверс родного Hypervisor.framework

Цель: понять, какие вызовы родной фреймворк делает в ядро, чтобы решить, реален ли kernel-перехват
(фронтенд A в `ARCHITECTURE.md`) и какой ABI надо повторить.

Все пути и команды ниже написаны по памяти и могут отличаться на твоей версии macOS. Проверяй.

## Что извлекаем

Фреймворки в macOS 11+ лежат не отдельными файлами, а в dyld shared cache. На Intel Sequoia он, как правило, тут:

```
ls /System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/
```

Файл вида `dyld_shared_cache_x86_64h`.

Извлечь можно, например, утилитой `dyld-shared-cache-extractor` или `ipsw dyld extract`
(ставятся отдельно, не входят в macOS):

```
mkdir -p reversing/dsc
dyld-shared-cache-extractor /System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_x86_64h reversing/dsc
```

Нужный бинарник: `.../System/Library/Frameworks/Hypervisor.framework/Versions/A/Hypervisor`.
Папка `reversing/` в `.gitignore`: **бинарники Apple в репозиторий не коммитим.**

## Что смотреть

1. `nm`/`otool -tV`: список экспортов `_hv_*` и то, куда они переходят.
2. В дизассемблере (Ghidra или Hopper) найти, как `hv_vm_create`, `hv_vcpu_run` обращаются к ядру:
   mach trap, syscall, ioctl или IOKit user client. От ответа зависит весь вариант A.
3. Структуры, которые передаются в ядро (размеры, смещения), и коды возврата.
4. Дальше ядро: Kernel Debug Kit для твоей сборки (если доступен) или `kmutil`/`nm` по ядру,
   поиск символов с `hv`. Это покажет, как называется реализация на стороне ядра.

## Что записывать

Складывай находки в `docs/reversing-notes/<macos-build>.md` (без кусков бинарников): список вызовов,
сигнатуры, номера trap/syscall, версия macOS, где расходится с предыдущей версией.

## Правовое замечание

Изучение интерфейса для совместимости на своём собственном железе обычно допустимо, но не копируй
код Apple в проект и не публикуй извлечённые бинарники. За точной оценкой для твоей юрисдикции
обращайся к юристу.
