---
name: reactos-display-restore
description: Use when fixing ReactOS fullscreen resolution restore, DISPRESTORE baseline, display mode re-assert, PDEV switch faults with NVIDIA drivers.
---

# ReactOS Display Restore — volver a 1366x768 tras Halo 800x600

Cubre #20, #22, #23 (trilogía del 800x600 atascado) + #30 PDEV SEH + #31 cursor.

## 1. Síntoma

Halo cambia a 800x600, al salir el escritorio queda en 800x600 +
`Failed to restore display mode` (`display.c:947`). Tres causas encadenadas,
tres fixes. No basta uno solo.

## 2. #20 fallback al modo guardado (PERMANENT) — `win32ss/user/ntuser/display.c`

Causa: `UserDisplayNotifyShutdown(NULL)` leía `DefaultSettings.X/YResolution`
del registro; en esta máquina no existen → `BADMODE`.
Fix: al primer `CDS_FULLSCREEN` guardar modo escritorio (`gdmRestoreDesktop` +
`gbRestoreDesktopValid`); el restore lo usa si el registro viene sin resolución.
Un cambio normal posterior lo invalida.

## 3. #22 re-assert aunque coincida — mismo fichero

Causa: el record coincidía pero el hardware divergía vía DXG
(`matches → skip` nunca re-afirmaba).
Fix: restore (`pdm==NULL`) NO salta aunque coincida: fuerza
`PDEVOBJ_bSwitchMode` (re-assert; el éxito limpia `gpFullscreen` vía
`UserUpdateFullscreen`, matando el `Failed` residual).
Forense (TEMP-DEBUG el log, PERMANENT el re-assert):
`DISPRESTORE restore candidate WxH vs current WxH` +
`DISPRESTORE re-asserting matching mode WxH`.

## 4. #23 baseline desde arranque (PERMANENT, la causa real)

Causa real: el guardado leía `pdmwDev` DESPUÉS de que el juego ya lo cambiara
(plain switch previo al primer FULLSCREEN) → guardaba 800x600 creyendo que era
escritorio.
Fix:
- `UserSeedDesktopMode(pdm)` siembra baseline desde el probe de defaults del
  driver (1366x768, estático en runtime) en `MDEVOBJ_Create(!pdm)`
  (`win32ss/gdi/eng/mdevobj.c`). Log `DISPRESTORE seeded desktop 1366x768`.
- FULLSCREEN solo guarda si NO hay baseline (re-entradas no pisan).
- Plain switches NO invalidan; solo `CDS_UPDATEREGISTRY` exitoso resetea.
- Log entrada: `DISPRESTORE saved desktop WxH bpp N`.

Secuencia sana: `seeded 1366x768` al arrancar → `restore candidate 1366x768
vs current 800x600` al salir → switch real de vuelta.

## 5. #30 PDEV SEH wrapper (TEMP-HARDEN) — `win32ss/gdi/eng/pdevobj.c`

`0x50` en `nv4+0x2102ef` tras doble switch (UAF del driver, pool liberado).
`PDEVOBJ_bSwitchMode` partido en `Unsafe` + wrapper:
```c
SavedApcDisable = ...CombinedApcDisable; // ULONG, no SHORT
_SEH2_TRY { retval = PDEVOBJ_bSwitchModeUnsafe(ppdev, pdm); }
_SEH2_EXCEPT(...) { restore APC; bFault=TRUE; retval=FALSE; }
_SEH2_END;
if (bFault) { _SEH2_TRY { PDEVOBJ_vEnableDisplay(ppdev); } _SEH2_EXCEPT(...) {} }
```
Semáforos (`hsemDevLock`, `ghsemPDEV`) siempre liberados; APC preservado.
Un fault aborta el switch con restore best-effort en vez de bugcheck.

## 6. #31 cursor Win32 (pendiente decidir fix)

Halo nunca adquiere mouse por dinput → ClipCursor + absolutas sin recentrar.
Traza en `win32k` (`cursoricon.c`): `GAMING-MOUSE ClipCursor (l,t)-(r,b)` /
`unclip` (raro, siempre) + `GAMING-MOUSE SetCursorPos #N (x,y)` (3 primeras +
1/240). Clip visto `(43,24)-(1323,744)` repetido + `hook 1` = movimientos
físicos, no recentrado. Pendiente: recentrado asistido o
`MouseWarpOverride=force`. Pedir 30 s moviendo ratón antes de decidir.

## 7. Confirmación

Salir de Halo en completa → escritorio 1366x768 solo, sin tocar nada;
`restore candidate 1366x768 vs current 800x600` + re-assert en log;
sin `Failed to restore` residual (si queda, cosmético, afinar después).
