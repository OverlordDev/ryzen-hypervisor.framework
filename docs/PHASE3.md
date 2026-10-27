# Фаза 3: VMX-совместимость (Hypervisor.framework для приложений)

Цель: приложение, написанное под Intel `Hypervisor.framework`, работает на Ryzen без изменений. Первый
тест: QEMU с `-accel hvf` грузит Linux. Цель дальше: Parallels Desktop, VMware Fusion, Docker Desktop.

## Что реально нужно (по заголовкам SDK 26.2)

Публичный x86 API намного шире, чем «VMCS + регистры». Три слоя:

| слой | функции | кто использует |
|---|---|---|
| **ядро API** | `hv_vm_create/destroy/map/unmap/protect`, `hv_vcpu_create/destroy/run`, `hv_vcpu_read/write_register`, `hv_vmx_vcpu_read/write_vmcs`, `hv_vmx_read_capability`, `hv_vcpu_read/write_msr`, `hv_vcpu_enable_native_msr`, `hv_vcpu_read/write_fpstate`, `hv_vcpu_interrupt`, `hv_vcpus_exit`, `hv_vm_sync_tsc` | QEMU (эмулирует APIC/IOAPIC в userland), большинство простых VMM |
| **встроенные устройства** | `hv_vcpu_apic_*`, `hv_vm_ioapic_*`, `hv_vm_atpic_*`, `hv_vm_lapic_msi`, `hv_vm_lapic_set_intr`, `hv_vm_add_pio_notifier`, `hv_vcpu_add_io_notifier` | новые коммерческие VMM (вероятно Parallels/VMware), экономят выходы |
| **продвинутое** | `hv_vm_space_*`, `hv_vcpu_set_space`, `hv_vcpu_vmx_set_shadow_access`, `hv_vmx_vcpu_*_shadow_vmcs`, вложенная виртуализация | специальные случаи |

**Честно про Parallels:** это закрытый бинарник. Я не знаю, какое подмножество он вызывает, использует ли он встроенные
APIC/IOAPIC (скорее всего да, раз API появился) и нет ли у него собственных проверок. Узнать можно только на нём
самом (`nm -u` по бинарнику, трассировка шимом). Поэтому первая цель QEMU: он открытый и использует только ядро API.

## Этапы

1. **Чистая трансляция VMCS ⇄ VMCB (сделано и проверено, `VmxMap.hpp`, `tests/vmx_map_test.cpp`).** Список из 166 полей VMCS
   (сверен с SDK), перевод прав доступа сегментов, перевод кодов выхода SVM в причины выхода VMX с квалификацией
   (I/O, MSR, EPT-нарушение из NPF, исключения, CR/DR).
2. **Состояние vCPU в ядре (сделано и проверено на железе: `rhv-vmx-test` печатает `PASS`).** `VmcsState.hpp`:
   тень VMCS (значение для каждого из 166 полей), запись и чтение состояния прямо в VMCB, применение управляющих
   полей к перехватам (`CPU_BASED`, `EXC_BITMAP`, `CR0/CR4` маски, окно прерываний, `WBINVD`), `EFER` (SVME скрыт от
   клиента, `LMA` следует за entry-контролем), `Interruptibility`, однократная инъекция события, запись причины
   выхода, квалификации, длины инструкции, адреса EPT-нарушения. Новые вызовы ядра `VmcsRead`/`VmcsWrite`, флаг
   `RHV_VMCREATE_VMX`. **Ещё нет:** `CR0/CR4` read shadow (чтение возвращает реальное значение), NMI-window,
   битмапы MSR/IO от клиента (мы перехватываем все MSR и, при включённых выходах по I/O, все порты).
3. **Реальные гости в ядре.**
   - **Сделано и проверено на железе (`rhv-long-test` PASS, 9 таймслайсов за ~100 мс гостя):** подмена x87/SSE/AVX и `XCR0` вокруг `VMRUN` (`FpuState.hpp`,
     `XSAVE`/`XRSTOR` с обработкой `CR0.TS`), вызовы `VmGetFpu`/`VmSetFpu` (формат как у `hv_vcpu_read_fpstate`),
     самопроверка в ядре (счётчик `vmFpuMismatches` должен быть 0), гость в long mode с paging. Аппаратный `XCR0`
     гостя всегда включает x87 и SSE, иначе `XSAVE` не сохранял бы XMM у гостя до первого `XSETBV`.
   - **Ещё нет:** `TSC_AUX`, MSR хоста и гостя (`KERNEL_GS_BASE`, `STAR` и т. д. переключаются `VMSAVE`/`VMLOAD`, остальные
     нужно проверить), несколько vCPU, `hv_vcpus_exit`/`hv_vcpu_interrupt` из другого потока, спекулятивные
     защиты (IBPB и др.), `hv_vcpu_enable_native_msr`.
4. **Шим (сделано и проверено на железе: `hvf-smoke` PASS через `scripts/run-with-shim.sh`).** `shim/rhv_core.c` — семантика API поверх абстрактного бэкенда
   (проверяется на хосте с поддельным ядром: `tests/shim_core_test.c`, плюс сверка всех констант с SDK); `rhv_iokit.c` —
   бэкенд к кексту; `hv_shim.c` — подмена через dyld interpose; `hv_stubs.inc` — сгенерированные заглушки
   `HV_UNSUPPORTED` для всего остального x86 API, чтобы вызов не уходил в настоящий фреймворк.
   Реализовано: `hv_vm_{create,destroy,map,unmap,protect,sync_tsc}`, `hv_vcpu_{create,destroy,run,run_until,interrupt,
   read/write_register,read/write_fpstate,read/write_msr,enable_native_msr,enable_managed_msr,flush,invalidate_tlb,
   get_exec_time}`, `hv_vmx_vcpu_{read,write}_vmcs`, `hv_vmx_read_capability`. Ограничения: одна ВМ на процесс, один
   vCPU; ядро пока пускает только root; `hv_vcpu_interrupt` срабатывает на ближайшем прерывании хоста (до ~10 мс).
   Запуск: `scripts/run-with-shim.sh ./build/hvf-smoke`.
5. **Свой дымовой тест на публичном API** (`tools/hvf-smoke`, написан): тот же бинарник работает на настоящем Intel
   Mac и на Ryzen через шим. Он же регрессионный тест на будущее.
6. **QEMU.** Собрать из исходников с `--enable-hvf` (на машине нет brew), загрузить Linux.
7. Встроенные APIC/IOAPIC/ATPIC и всё, что потребует Parallels.

## Риски

- Этап 3 это самый опасный код проекта (FPU и прерывания в ядре): каждая ошибка это паника.
- Семантика VMX и SVM различается в мелочах (`CR0/CR4` shadow, NMI blocking, `Activity state`, `preemption timer`).
  Приложения иногда на них полагаются. Всё, что мы не можем точно эмулировать, нужно явно документировать.
- Нет гарантии, что Parallels не использует приватные вызовы или проверки, нарушаемые подменой.
