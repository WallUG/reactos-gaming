---
name: reactos-teardown-estabilidad
description: Use when fixing ReactOS teardown crashes, VRAM drain, page table leaks, DRIVEROBJ cleanup, stale device handles, FPU TS faults, APC imbalance with NVIDIA drivers.
---

# ReactOS Teardown y Estabilidad — fixes que evitan que la salida mate la sesión

Cubre #1, #3, #6, #8, #9, #11, #13, #14, #16, #17, #19 (y #24 higiene).

## 1. #1 KiTrap07 TS fix (PERMANENT) — `ntoskrnl/ke/i386/traphdlr.c`

Bug: tras cargar estado FPU, reanudaba con TS puesto
(`Cr0 |= SaveArea->Cr0NpxState` + retorno condicional) → re-faulteo inmediato:
bucle #NM en kernel (`fxsave` en `Ke386SaveFpuState`, 475× visto), doble falta
en usuario; 3 hilos = livelock + `is hung`.
Fix:
```c
Cr0 = __readcr0();
Cr0 &= ~CR0_TS;
Cr0 |= (SaveArea->Cr0NpxState & ~CR0_TS);
__writecr0(Cr0);
_enable();
KiEoiHelper(TrapFrame);
```
Nunca reanudar con TS. `#2 FPUTRAP DPRINT1→DPRINT` acompaña (cada falta ~10 ms).

## 2. #3 MDL boundary (PERMANENT) — `ntoskrnl/mm/ARM3/mdlsup.c`

`MiUnmapLockedPagesInUserSpace`: solo `MiDeletePde` si
`MiIsPteOnPdeBoundary(PointerPte+1) || NumberOfPages==1`. Si no, omitir
(fuga segura vs UAF). Cazado por `ASSERT MiIsPteOnPdeBoundary` en vivo.

## 3. #8 NpxThread cleanup (PERMANENT) — `ntoskrnl/ps/kill.c` `PspExitThread`

```c
if (KeGetCurrentPrcb()->NpxThread == &Thread->Tcb) {
    KeGetCurrentPrcb()->NpxThread = NULL;
    Thread->Tcb.NpxState = NPX_STATE_NOT_LOADED;
}
```
Estado FPU colgando tras muerte de hilo (ruta Halo+WMI). #9 APCTRACE
(`apc=%d` al entrar y antes del assert `kill.c:841`) distingue leak de vida
vs teardown. #24 lo pasa a `DPRINT`.

## 4. #19 VRAM drain + ShareCount — `ntoskrnl/mm/ARM3/procsup.c`

VAD `VadDevicePhysicalMemory` (VRAM 64 MB `[14330000-1832FFFF]` type=1):
el path genérico bugcheckea en PFN (`0x1A/0x401`). Rama propia: borra PTEs 4K
directas con PFN lock + `KeFlushCurrentTb`, log
`dropped device-physical VAD [...](N erased, M skipped)`.
Obligatorio por PTE: `MiDecrementShareCount` (como `MiDeletePteRange`) +
`MiDecrementPageTableReferences` → `MiDeletePde` si 0. Sin lo primero,
assert `ShareCount==1` (`virtual.c:511`).

## 5. Safety net + PDETBL + VADCLEAN (attach-aware)

- `VADCLEAN pid=<victima> type priv ca [start-end]`: cada VAD visitado.
- `PDETBL pid=<victima> idx va share ref validptes first proto firstpte`:
  vuelca PDEs válidas. ¡PID víctima (`Process->UniqueProcessId`), no ejecutor!
- Safety net en `MmDeleteProcessAddressSpace` (punto más tardío con tablas):
  por tabla, sin skip-ahead; mapea paged-out con `MiMakeSystemAddressValid`;
  tablas vacías → `MiDeletePde` directo (el genérico solo libera por dec→0:
  leak silencioso en release, assert en debug — fix #13 PERMANENT);
  prototipos se borran directos; resto → `MiDeleteVirtualAddresses`.
- Contexto: `PspDeleteProcess` suele ir attached a la víctima aunque ejecute
  System. Test WRONG: `PsGetCurrentProcess()==Process`. Test correcto:
  ejecutor==víctima o attached-TO-víctima. Attach temporal si foreign+passive+
  unattached+APCs enabled; si no, `SKIPPED` (mentiría). Nunca dereferenciar
  `Peb`. Locks: WS con `KeEnterGuardedRegion` en contexto attached; PFN dentro
  de WS; prints ANTES de PFN. Log `safety net pid=... swept N proto, M tables`.

## 6. #6 DRIVEROBJ SEH (TOLERANCE) — `win32ss/gdi/eng/driverobj.c`

```c
ULONG SavedApcDisable = PsGetCurrentThread()->Tcb.CombinedApcDisable;
_SEH2_TRY { pFreeProc(&pedo->drvobj); }
_SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
    PsGetCurrentThread()->Tcb.CombinedApcDisable = SavedApcDisable;
}
```
Cleanup nv4 (`+0x210b2a` → `EngDeviceIoControl` con device muerto →
`0xC0000005` en `IoBuildDeviceIoControlRequest`) no debe tumbar la sesión.
v1 forzaba a 0 y rompía `resource.c:1863`; v2 guarda/restaura; v2b
SHORT→ULONG (campo son DOS contadores; SHORT truncaba guarded `0xFFFF0000`).
Log `calling free proc %p obj %p dhpdev %p hdev %p (tid %lx)` + `faulted, code`.

## 7. #11 stale-handle guard (TOLERANCE) — `win32ss/gdi/eng/device.c`

```c
if (!MmIsAddressValid(DeviceObject) ||
    DeviceObject->Type != IO_TYPE_DEVICE ||
    DeviceObject->StackSize < 1)
    return ERROR_INVALID_HANDLE; // + ERR con caller
```
SEH no traga bugchecks (`0x35` con `StackSize` 0). NO chequear `Size`
(`IoCreateDevice` suma extensión → mataba arranque `0xB4`).

## 8. #14 orden GDI (PERMANENT) — `win32ss/gdi/ntgdi/gdiobj.c`

Dos pasadas en `GDI_CleanupForProcess`: primero DRIVEROBJs, luego resto.
Callbacks del driver leen hermanos (sesión); si murieron primero → `0x50`
a dirección kernel → `MmAccessFault` bugcheckea SIN dispatch: ningún SEH lo
traga, solo el orden lo evita.

## 9. #12 backoff + #17 GDIDESTROY

- #12 (PERMANENT): `EngpUpdateGraphicsDeviceList` backoff 60 s a VideoN con
  CREATE `0x22` (Afterburner hot-loop).
- #17: `GdiProcessDestroy` desmapea `GdiSharedHandleTable` + `GDIDESTROY`
  con dirección (candidata idx498). PERMANENT si confirma.

## 10. Señales de confirmación

`dropped device-physical (N erased)` + `PDETBL pid=<halo>` + `safety net swept`
sin assert; `DRIVEROBJ_vCleanup: calling free proc` sin bugcheck posterior;
`APCTRACE apc=0` en todos los hilos; `stale device handle rejected` barato.
