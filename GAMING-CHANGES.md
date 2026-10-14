# ReactOS Gaming — registro de cambios

Objetivo: base ReactOS capaz de ejecutar juegos 3D con driver NVIDIA real
(GT 710, 353.62). Hito: Halo campaña jugable en 3D.

Árboles: `LH` = Longhorn-ReactOS-Stable (Linux) · `WDDM` = reactos-Wddm-Win32k-Branch (Windows)
Estados: [PERMANENT] va a la publicación · [TEMP-DEBUG] revertir antes de publicar ·
[TOLERANCE] frontera ante drivers cerrados · [PARKED] aparcado con motivo

## 1. Fixes del arco gaming (esta sesión)

| # | Fichero (árbol) | Cambio | Por qué | Estado |
|---|---|---|---|---|
| 1 | `ntoskrnl/ke/i386/traphdlr.c` (LH) | `KiTrap07Handler`: reanudar siempre con TS limpio (`Cr0 \|= Cr0NpxState & ~TS`, retorno incondicional) | Reanudar con TS re-faultea inmediato: bucle #NM cerrado en modo kernel (`fxsave` en `Ke386SaveFpuState`, 475× visto), doble falta en usuario; con 3 hilos = livelock + "is hung" | [PERMANENT] |
| 2 | `ntoskrnl/ke/i386/traphdlr.c` (LH) | `FPUTRAP`: `DPRINT1` → `DPRINT` | Cada falta costaba ~10 ms de serie; la tormenta convertía ms en minutos | [TEMP-DEBUG] revertir o mantener degradado |
| 3 | `ntoskrnl/mm/ARM3/mdlsup.c` (LH) | `MiUnmapLockedPagesInUserSpace`: `MiDeletePde` condicional al boundary | `ASSERT MiIsPteOnPdeBoundary` cazado en vivo por serie | [PERMANENT] |
| 4 | `win32ss/user/winsrv/usersrv/init.c` (LH) | `SrvGetThreadConsoleDesktop`: impresión eliminada | nvoglnt sondea `GetThreadDesktop` en hot path (6.5K llamadas); cada fallo imprimía por serie 115200 → csrss 99% + display muerto | [TEMP-DEBUG] el fix correcto (fallback en servidor) está [PARKED]: 0 SERVERFAIL + fallo by-design sin efecto semántico |
| 5 | `win32ss/user/user32/misc/desktop.c` (LH) | caller-ID rate-limited (8/proceso, `ERR`) + rama `SERVERFAIL` | Identificar al sondeador: nvoglnt (`69D9871C`), éxito siempre (`desk=144`) | [TEMP-DEBUG] revertir |
| 6 | `win32ss/gdi/eng/driverobj.c` (LH) | `DRIVEROBJ_vCleanup`: `_SEH2_TRY/EXCEPT` + save/restore **ULONG** de `CombinedApcDisable` + TID en el log | Cleanup de nv4_disp (`+0x210b2a` → `EngDeviceIoControl` con device muerto → `0xC0000005` en `IoBuildDeviceIoControlRequest`) tumbaba la sesión al salir del juego. v1 forzaba el contador a 0 y rompía el invariante inverso (región legítima de win32k → break en `resource.c:1863`); v2 guarda/restaura; v2b corrige `SHORT`→`ULONG` (el campo son DOS contadores: low=kernel, high=guarded; el SHORT truncaba un guarded retenido `0xFFFF0000`) | [TOLERANCE] frontera ante driver cerrado |
| 7 | `ntoskrnl/ke/hbd.c` (LH) | DPC heartbeat cada ~2 s (`tick`, thread, IRQL por serie) | Prueba de vida del kernel con display muerto; distinguió cuelgue-kernel de cuelgue-presentación | [TEMP-DEBUG] revertir |
| 8 | `ntoskrnl/ps/kill.c` (LH) | `PspExitThread`: limpia `NpxThread` del PRCB si es el hilo saliente | Estado FPU colgando tras muerte de hilo (ruta Halo+WMI) | [PERMANENT] |
| 9 | `ntoskrnl/ps/kill.c` (LH) | `APCTRACE`: log de `CombinedApcDisable` al entrar a `PspExitThread` y antes del assert `kill.c:841` | Cazar quién filtra el disable: nonzero al entrar = leak de vida del hilo; solo antes del assert = leak del teardown | [TEMP-DEBUG] revertir |
| 10 | `win32ss/gdi/eng/pdevobj.c`, `bitblt.c`, `copybits.c`, `ntgdi/wingl.c` (LH) | `DDITRACE` enter/exit en `DrvEnableSurface` / `DrvAssertMode` / `DrvDisableSurface` / `DrvBitBlt` / `DrvCopyBits` / `DrvDescribePixelFormat(+max)` / `DrvSetPixelFormat` / `DrvSwapBuffers` | El último `enter` sin `exit` delata el DDI donde muere nv4_disp (`+0x2116DF`, `0x50` en transición a 3D). Veredicto parcial: switch + blts completan; GL-DDIs casi sin uso (Halo va por ICD); sospecha actual: `MapMemory` de dxg (#15) | [TEMP-DEBUG] revertir |
| 15 | `win32ss/reactx/dxg/ddsurf.c` (LH) | `DDITRACE` en `MapMemory` del driver (vía `DbgPrint`) | El crash cae tras churn MDL sin frame win32k: dxg.sys llama al driver DIRECTO. `MapMemory` mapea VRAM al proceso justo tras el switch y lee linkage PDEV que puede estar rancio → `0x9D009C00` constante | [TEMP-DEBUG] revertir |
| 16 | `ntoskrnl/mm/ARM3/procsup.c` (LH) | `PDETBL` + `VADCLEAN` + safety net, con PID de la VICTIMA (`Process->UniqueProcessId`, no ejecutor) | El log global entrelaza procesos Y el teardown puede correr en hilo ajeno (System limpiando a otro): el PID ejecutor mentia. Pendiente: `PDETBL pid=4` con idx0+idx498 - victima real por confirmar (apagado vs salida de Halo) | [TEMP-DEBUG] revertir |
| 17 | `win32ss/gdi/ntgdi/init.c` (LH) | `GdiProcessDestroy`: desmapea `GdiSharedHandleTable` + log `GDIDESTROY` con su direccion (en contexto, seguro) | Asimetria mapear/liberar; candidata a tabla zombie (la direccion dira si es idx498) | [PERMANENT] si se confirma |
| 11 | `win32ss/gdi/eng/device.c` (LH) | `EngDeviceIoControl`: valida el handle (`MmIsAddressValid` + `Type` + `StackSize`) antes de tocarlo | El cleanup del driver pasaba un device muerto: una vez fault en `IoBuild` (`0xC0000005`), otra bugcheck `0x35` (`StackSize` 0). El SEH no traga bugchecks; el chequeo sí los evita. v1 incluía `Size == sizeof(DEVICE_OBJECT)` y mataba el arranque (`0xB4`): `IoCreateDevice` suma la extensión al `Size`. Solo `Type`+`StackSize` | [TOLERANCE] validación de frontera |
| 12 | `win32ss/gdi/eng/device.c` (LH) | `EngpUpdateGraphicsDeviceList`: backoff 60 s a devices que fallan al registrar | `Video1` (2º head, CREATE denegado `0x22`) nunca entraba en lista → cada `EnumDisplayDevices` (Afterburner/monitores lo sondean en hot-loop) reintentaba el open + print → miles de líneas + CPU. Con backoff el sondeo es barato; el print genuino queda | [PERMANENT] |
| 13 | `ntoskrnl/mm/ARM3/procsup.c` (LH) | Safety net libera TABLAS VACIAS (`MiDeletePde` si 0 PTEs validos) | Hallazgo: el path generico solo libera tablas via dec->0 por PTE borrado; las tablas que quedan vacias (todas las de un TerminateProcess) no las libera nadie -> refs al directorio -> assert. Solo visible en debug (leak silencioso en release) | [PERMANENT] fix de leak real |
| 14 | `win32ss/gdi/ntgdi/gdiobj.c` (LH) | `GDI_CleanupForProcess`: destruye DRIVEROBJs PRIMERO (dos pasadas) | Causa raíz de los `0x50` de salida: el cleanup iba en orden de handle y el callback del driver leía objetos hermanos ya liberados (sesión). Clave: esos faults son a dirección kernel → `MmAccessFault` hace bugcheck directo SIN dispatch: ningún SEH del mundo los traga; solo el orden los evita | [PERMANENT] fix de orden real |

## 2. Trabajo previo habilitante (heredado al inicio de sesión)

- KSE portado (`ntoskrnl/kse/{kse,driverscope,versionlie}.c`, `kse.h`, `drvmain.xml/sdb`, hooks en driver/iomgr/sysldr) → driver 353.62 carga. [PERMANENT]
- Build fixes GCC 8.4 (mayúsculas WIDL, `C_ASSERT`, vtbls d2d1/dwrite, exports ntdll/psapi, `ksecdd`, `Dxgk.c`, MediaFoundation/ffmpeg/winedmo/colorcnv/wmvdecod). [PERMANENT]
- Perfil español: `IDS_LOCALLOWAPPDATA/IDS_ROAMINGAPPDATA` + `CreateStandardProfile` crea intermedios (árbol WDDM). [PERMANENT]

## 3. Herramientas de laboratorio (no se publican con el OS)

- `rx.c` / `xmsend.py`: receptor XMODEM-CRC en ReactOS + emisor Python; `rx stage` agenda canje vía `PendingFileRenameOperations`. Plan B ante serie-RX rota.
- `kdbwatch.sh`: vigilante que ante prompt KDB inyecta `b`→`bt`→`cont` (requiere serie TX viva).

## 4. Mapa de despliegue (binario → contenido → destino)

| Binario | Lleva (#) | Destino target |
|---|---|---|
| `ntoskrnl_FPUFIX.exe` | 1, 2, 7, 8 | `C:\` + entrada freeldr (sustituido por APCDBG) |
| `ntoskrnl_APCDBG.exe` (+`GAMING-BUILD <fecha>`) | 1, 2, 7, 8, 9, 13, 16, 18 | `C:\` + entrada freeldr. LECCION: el reloj WSL invierte mtimes y ninja salta recompilas (binarios rancios con cara de frescos); verificar por `strings`, no por hash/fecha. Forzar con `touch` ante duda |
| `winsrv_SILENT.dll` | 4 | `C:\ReactOS\system32\winsrv.dll` |
| `user32_CALLER2.dll` | 5 | `C:\ReactOS\system32\user32.dll` |
| `win32k_CLEANUP.sys` | 6, 10, 11, 12, 14 | `C:\ReactOS\system32\win32k.sys` |
| `dxg_TRACE.sys` | 15 | `C:\ReactOS\system32\drivers\dxg.sys` |

## 5. Pendientes

- [ ] Crash salida: verificar salida limpia con #6 (modo 800x600: `Failed to restore display mode` es bug separado, `display.c:947`).
- [ ] Rendimiento 3-5fps: no es el serie (igual sin debug). Candidatos: wined3d sin FBO (`GL_FRAMEBUFFER_UNSUPPORTED` por frame), checked build, uniprocesador. Experimentos: Taskmgr %CPU, 640x480.
- [ ] `STOP 0x50` en `nv4_disp+0x2116DF` (`cmp (%edi),%eax`, `edi=[param1+0x338]=0x9D009C00`) en mode-switch: determinista en stack viejo, pendiente de reproducir en stack nuevo.
- [ ] Serie RX del target muerta (cable+adaptador sanos; socket DB9 del target). Plan: otra PC.
- [ ] Antes de publicar: revertir todo [TEMP-DEBUG] (2 parcial, 5, 7) y decidir #4 (restaurar print o dejar fallback).
| 18 | `ntoskrnl/ke/hbd.c` (LH) | `GAMING-BUILD __DATE__ __TIME__` al arrancar | Los logs no probaban que binario corria (frankenkernels); el marker lo hace trivial | [PERMANENT] |

## #19 Fullscreen Halo: drenaje VRAM con ShareCount (procsup.c)
- Rama device-physical borraba PTEs sin `MiDecrementShareCount` de la tabla -> `MiDeletePde`/`MiDeletePte` assert `ShareCount==1` (virtual.c:511) en VAD 64MB `[14330000-1832FFFF]` type=1. Fix: decrementar ShareCount por PTE como `MiDeletePteRange`/mdlsup. TEMP-DEBUG hasta salida completa limpia.

## #20 Restaurar resolucion al salir de fullscreen (display.c)
- `UserDisplayNotifyShutdown(NULL)` leia `DefaultSettings.X/YResolution` del registro; en esta maquina no existen -> `BADMODE` (`Devmode doesn't specify`) y el escritorio quedaba en el modo del juego. Fix: al primer `CDS_FULLSCREEN` se guarda el modo escritorio (`gdmRestoreDesktop`) y el restore lo usa si el registro no trae resolucion; un cambio normal lo invalida. PERMANENT (comportamiento Windows: volver al modo previo).

## #21 0x50 en nv4_disp free proc + traza forense (driverobj.c/device.c)
- Nuevo crash a la salida: `nv4_disp+0x210aa6` desde `DRIVEROBJ_vCleanup`, lee `0x9D00DBA4` (fuera de todo VAD del juego). El SEH no lo contuvo (bugcheck en `MmArmAccessFault`). Sospechas: (a) el free proc no chequea el error de `EngDeviceIoControl` stale-handle y persigue un puntero basura; (b) contexto dhpdev ya liberado. TEMP-DEBUG: log de `free proc/obj/dhpdev/hdev` antes de llamar + `caller` en el reject stale-handle, para ordenar causa/efecto en el proximo log.

## #22 Restore fuerza re-assert + forense DISPRESTORE (display.c)
- El record decia escritorio pero el hardware seguia en juego (divergencia via DXG): el `matches -> skip` nunca re-afirmaba el modo. Ahora el restore (`pdm==NULL`) NO salta aunque coincida: fuerza `PDEVOBJ_bSwitchMode` (re-assert; el exito limpia `gpFullscreen`). Forense `DISPRESTORE`: modo guardado a la entrada, candidato vs actual a la salida. TEMP-DEBUG el log, PERMANENT el re-assert si funciona.

## #23 Baseline desde el modo de arranque, no de la entrada fullscreen (display.c/mdevobj.c)
- Causa real del 800x600 atascado: mi guardado leia `pdmwDev` DESPUES de que el juego ya lo habia cambiado (plain switch previo al primer FULLSCREEN) -> guardaba 800x600. Fix: `UserSeedDesktopMode` siembra la baseline desde el probe de defaults del driver (modo de arranque 1366x768, estatico en runtime) en `MDEVOBJ_Create(!pdm)`; el FULLSCREEN solo guarda si no hay baseline; el plain ya NO invalida; solo `CDS_UPDATEREGISTRY` exitoso la resetea. PERMANENT.

## #24 Higiene de logs (procsup.c/kill.c)
- Manguera TEMP-DEBUG a `DPRINT` (fuera del binario): `VADCLEAN`, `PDETBL`, `safety net`, `APCTRACE` (~150 lineas por salida de proceso -> 0). Verificado por `strings`: ausentes, `GAMING-BUILD` intacto. Se quedan en `ERR` (raros y valiosos): `DISPRESTORE`, `stale device handle`, `driver free proc faulted/tolerated`, `GDIDESTROY`, heartbeat. Para re-depurar, revertir a `DPRINT1` y reconstruir.

---
## ESTADO FINAL FASE ESTABILIDAD (2026-09-11, verificado en GT 710 + 353.62)
Combo que juega Halo fullscreen, sale limpio, restaura 1366x768 y sobrevive:
- `ntoskrnl_APCDBG.exe` `569baf363eba68ade7258470f3c593fee6ba4e7d5c57f32d33473ea4f3f44ee6` -> `C:\` + freeldr
- `win32k_CLEANUP.sys` `83e5ff9869719e4c8d71beb1d9247a0b3b72ecd99ee48c8be614e9994c07100e` -> `system32\win32k.sys`
- `dxg_TRACE.sys` `daed3a06e459bea8b70a09c12fd3e2bd8b46e821cb660a667c49a3479161cb9c` -> `system32\drivers\dxg.sys`
- `winsrv_SILENT.dll` `21d10786b30c3df418a63a0780484dffcac1d8711285dcf12a10a938f8fc2fa7` -> `system32\winsrv.dll`
Sintomas cerrados: APC disable imbalance, VAD VRAM asserts (ShareCount), DRIVEROBJ free-proc 0x50 (guardia stale-handle), modo no restaurado (baseline de arranque + re-assert). Ruido serie reducido (#24).
Pendiente conocido: `nv4_disp` free proc a veces persigue punteros basura (nondeterminista, contenido por la guardia); `Video1 0xc0000022` (Afterburner, backoff); 3-5fps -> ver FASE RENDIMIENTO.

---
## FASE RENDIMIENTO
- #25 Medicion: `d3dwine` fps-meter (presents+draws). Present inocente (<1ms/frame, via GL). Draws ~400/frame a ~0.5ms c/u con CSMT on.
- #26 `HKCU\Software\Wine\Direct3D` DWORD `csmt=0` (single-threaded command stream): 5fps -> ~30fps estables en i3 monoprocesador. El handoff entre hilos por draw era el impuesto. DEFAULT GAMING; documentado, no binario.
- #27 VRAM=0 descartado: fallback a 1280MB de la DB no mueve fps. `card GTX470 10de:06cd`, HW vertex processing (`software 0x0`), GL 4.4 NVIDIA: todo el pipeline es hardware.
- Pendiente 30->60: frame clavado en ~33.5ms aunque los draws varien 335<->477 => espera fija (vsync a 60Hz cayendo cada 2 vblanks, o cap 30fps del propio Halo). Mirar ajuste Framerate/VSync del juego.

- #28 Hito 60fps: con `csmt=0` + ajustes bajos, 120 frames en ~2010ms = 59.7fps estables (vsync 60Hz; el present absorbe la espera: 400-900ms/120). Ley: fps ~= 1/max(draws*80us, 16.7ms): ~200 draws -> 60fps, ~400 -> 30fps, ~1000 -> 20fps. Pendiente: bajar coste/draw (build -O0) para 60fps en escenas pesadas.

- #29 Ratón FPS atrapado en bordes (dinput/mouse.c): `warp_check` solo recentraba UNA vez por acquire (`if (!clipped)`); despues el cursor vagaba dentro del clip hasta clavarse en un borde y los deltas morian (igual en ventana y completa). Fix: recentrar siempre en cada poll exclusivo (throttle 10ms) + mantener clip. Marker `GAMING-MOUSE exclusive acquire`. PERMANENT (mecanica FPS estandar).

- #30 Contencion de faults del driver en switches de modo (pdevobj.c): 0x50 en `nv4+0x2102ef` leyendo pool liberado tras doble switch consecutivo (UAF del driver, misma zona que el 0x50 del free proc). `PDEVOBJ_bSwitchMode` partido en `Unsafe` + wrapper SEH: un fault aborta el switch con restore best-effort y devuelve FALSE en vez de bugcheck; semaforos siempre liberados; APC-count preservado. TEMP-HARDEN hasta confirmar.
- #31 Ratón Win32 (no dinput): Halo nunca adquiere el mouse por dinput (teclado sí) -> usa ClipCursor + posiciones absolutas sin recentrar; el clip lo clava en bordes. Traza `GAMING-MOUSE` en `UserClipCursor`/`UserSetCursorPos` (win32k) para ver rects. Pendiente: decidir fix (p.ej. recentrado asistido o `MouseWarpOverride=force` si el juego tolera dinput).

---
## FASE SMP/APIC + HDA (GT 710, rama smp-port, Oct-2026)
Estado: desktop 1360x768 + NVIDIA (gráfica y HDMI-audio) funcionando en APIC SMP **con Realtek deshabilitado en BIOS** (andamio, no fix). Sin Realtek: cero storms, verbs HDA#1 OK, `DxEngLockHdev`.

### Árbol de fallos probado en HW real (H81 + GT 710)
1. **Resurrección de storms (A1)**: `USBPORT_TimerDpc` rearma cada vector held cada ~500ms (`usbport.c:1711-1721`) + probation global cada-1000 en el HAL (`apic.c` viejo) = re-floods de +1000 exactos cada ~250s. FIX **F1** (tag SMP-124): probes por vector rate-limitados (1200) + gate de línea quieta (`RemoteIRR==0`); verificado 8+ min sin re-flood.
2. **Ruteo GSI tras puentes (M1)**: GPU (bus1 dev0 INTA) → GSI 16 y HDA-NVIDIA (INTB) → GSI 17 según `_SB.PCI0.P0P2._PRT=AR02`; PnP daba IRQ 10/11 (`$PIR`) → ISRs muertas de hambre + floods en líneas USB (`b1` nack 4924/ack 32, `a1` nack 7840/ack 0). FIX **F2-1** (tag SMP-125): walk de puentes por config-space + quirk `P0P2` + gate `HalIsApicMode()` (en PIC, inerte: verificado, sin `PCIFIX`); verificado: verbs HDA#1 `0x102`→`0x0`, cero storms, desktop pinta.
3. **HDA#2-Realtek (dev 27)**: wedge duro de CPU en el primer `hda_write16(CORBWP)` — localizado por DIAG al write (lock OK, reads OK, sin loops en su ISR). Solo SMP-4 (UP y PIC pasan con el mismo binario); delay 20s no lo mueve → concurrencia, no settle. **ABIERTO** (ver F2-1b/d).
4. **USB#1 (dev 29)**: DxxIR dice 17 (tras freeze que lo fuerza, `PCIFRZ 6->1`), `_PRT` dice 23, `$PIR`/3Ch dice 10. Triple desacuerdo; su GSI verdadero pendiente de micro-experimento. No bloquea.
5. **HDA#1 bajo UP**: verbs `0x102` (bajo SMP-4 responde). Observación no bloqueante.
6. **`0x19 BAD_POOL_HEADER`** (`expool.c:279`, linkage, tag vecino `SeSd`) una vez en arranque de servicios ya en desktop; no repitió. En vigilancia (primera vez que APIC llegaba a servicios).

### Tabla de cambios (esta fase)
| # | Fichero | Cambio | Por qué | Estado |
|---|---|---|---|---|
| 32 | `hal/halx86/apic/apic.c` | F1: rearm seguro (sin lotería global; probe por vector + `RemoteIRR==0` + contadores `resurr`/`RearmCalls`); DIAG `IRQMAP`/`RTEEN`/`RTEMASK`(+reread)/`RTEARM`/`RTEDIS` | A1 + mapa IRQ→vector→RTE por boot | [PERMANENT] F1 + estado; prints REVERTIDOS (strip SMP-128) |
| 33 | `ntoskrnl/ke/i386/irqobj.c` | tick en prints storm; censo `ack`/`nack` por ISR encadenada; `ISRVEC` extendido | Probar quién reclama cada línea (USB niega 7840, IDE reclama todo) | [REVERTIDO en strip] |
| 34 | `ntoskrnl/io/iomgr/irq.c` | `IRQCONN ok obj/isr/vector` en éxito | Resolver VECSTAMP→driver (mapeó `b1`=USB#1, `a1`=USB#2) | [REVERTIDO en strip] |
| 35 | `ntoskrnl/ke/hbd.c` | `ISRCNS` cada ~60s vía `KiIsrDumpTotals()` | Cazar goteo bajo el umbral (A2) | [REVERTIDO en strip] |
| 36 | `drivers/bus/pci/pdo.c` | F2-0 walk de puentes + `PCIBRDG`; F2-1a `PCIDEV`/`PCIFRZ` + firma con `Slot`; F2-1 enforce `(1,0)`→AR02 + `PCIFIX` + gate APIC | M1: GPU→16/HDA→17; el gate evita IRQ 16+ en PIC | [PERMANENT-candidate] walk+enforce; prints REVERTIDOS (strip SMP-128) |
| 37 | `drivers/wdm/audio/hdaudbus/hdac_controller.cpp` | F2-1b prints por paso en `SendHDACmds`; F2-1c/d delay 20s ante verbo `0x200f0000` | Localizar wedge HDA#2 (es el write) + discriminar timing (descartado) + mute F2-2d (kept, sin prints) | [PARCIALMENTE REVERTIDO: prints y delay fuera; mute queda] |

### Mapa de despliegue (disco PC real, verificado por SHA)
- `ntoskrnl.exe` (=`ntkrnlmp` F1) `76f73e3d…` + `hal.dll` (=`halmacpi` F1) `43b752c5…` + `pci.sys` (F2-1+gate) `0f9040a5…` + `hdaudbus.sys` (F2-1b/d) `f1fed7cb…` → `C:\ReactOS\system32[\drivers]`
- Spares: `ntoskrnl-UP-T5.exe`, `hal-PIC-T5.dll` (boot PIC por renombrado); backups `.preT0`/`.preF2*` en el mismo dir.
- Logs serie en `\\wsl…\home\demox\T0|T5|F1|F2-0|F2-1a|F2-1b|F2-1c|F2-1d|F2-1|E1|E2-*.txt`; tablas ACPI en `acpi/` (MADT: 1 IOAPIC, overrides solo 0→2 y 9→9).

### Pendientes (orden)
- [x] Strip serie SMP-128: fuera todos los `DbgPrint` TEMP (verificado 0 markers en los 4 binarios); quedan F1, F2-1+gate, mute F2-2d. DIAG preexistente de rama intacto.
- [ ] HDA#2 sordo bajo APIC (verbs `0x102`, su ISR en `81` no entrega) con ambos drivers: no bloquea (boot sigue). Siguiente capa.
- [ ] HDA#2 wedge (CORBWP) con mute levantado: pendiente de retest con binarios limpios (el wedge se vio con DIAG; confirmar que el mute basta).
- [ ] dev29: probar GSI 22/23 y ver acks.
- [ ] soak 15 min (rama `skip-busy` de F1 sin ejercitar).
- [ ] `0x19`: en vigilancia (un run, no repitió).
- [ ] Disco lleva binarios CON prints (pre-strip): refrescar kernel+hal+pci+hdaudbus en próximo toque.
- [ ] Antes de publicar: decidir PERMANENT de F1/F2-1/mute; F2 genérico = evaluar `_PRT` (nadie lo llama hoy: `acpica/resources/rsxface.c:174`).
