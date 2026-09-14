---
name: reactos-kernel-forense
description: Use when debugging ReactOS kernel crashes via serial logs, KDB, BSOD photos, disassembly of nv4_disp, FPUTRAP, heartbeat, DDITRACE, APCTRACE, PDETBL. Forensic methodology without interactive debugger.
---

# ReactOS Kernel Forense — depurar sin debugger interactivo

## 1. Cuándo usar

Crashes/BSOD/cuelgues en ReactOS target con driver cerrado (nv4_disp/nvoglnt),
serie unidireccional, sin KDNET. Síntomas: `STOP 0x50/0x1E/0xA`,
`is hung`, display muerto, csrss 99%, prompts `kdb:>`.

## 2. Infraestructura

- Serie: `usbipd list/bind/attach --wsl` → `/dev/ttyUSB0` 115200
  `cs8 -cstopb -parenb -ixon raw`, `chmod 666`. Captura
  `/home/demox/serial-caps/cap1.log`. Entrada ReactOS `(Debug)`
  `DEBUG DEBUGPORT=COM1 BAUDRATE=115200 SOS`. Entrada no-debug para medir
  fps real y para transferencias XMODEM.
- `kdbwatch.sh`: ante `kdb:>` inyecta `b`→`bt`→`cont` (solo observa si RX muerto).
- KDNET aparcado: `kdnet.dll` carga pero `KdDebuggerInitialize0` jamás se llama;
  kernel solo Serial/Screen/File. Wireshark `udp.port==50000 or bootp` vacío en
  arranque = prueba. No reabrir sin piezas nuevas.
- Cable: null-modem completo hembra-hembra (cruce 2↔3 + 7↔8). Género ≠ cruce.
  Test: continuidad pin2→pin3 = null-modem; pin2→pin2 = recto inútil.
  Puente 2-3 + PuTTY eco prueba el adaptador.

## 3. Instrumentación estándar (toda TEMP-DEBUG y marcada)

- `FPUTRAP` en `ntoskrnl/ke/i386/traphdlr.c:739` (`KiTrap07Handler`):
  `DPRINT("FPUTRAP: Eip=%08lx Irql=%lu CR0=%08lx Thread=%p NpxThread=%p NpxState=%lx")`.
  v2: `KeBugCheckEx(0x46505554,...)` si IRQL>DISPATCH (foto identifica EIP).
- `HEARTBEAT` en `ntoskrnl/ke/hbd.c:39`: DPC 2 s
  `HEARTBEAT: tick=N Thread=%p Irql=%u`. Siguen = display/input muertos.
  Se paran = wedge. + `GAMING-BUILD __DATE__ __TIME__` en `hbd.c:58`.
- `DDITRACE enter/exit` en `DrvEnableSurface/AssertMode/DisableSurface/BitBlt/
  CopyBits/DescribePixelFormat/SetPixelFormat/SwapBuffers`
  (`win32ss/gdi/eng/pdevobj.c,bitblt.c,copybits.c,ntgdi/wingl.c`) +
  `MapMemory` en `win32ss/reactx/dxg/ddsurf.c` vía `DbgPrint`
  (dxg llama al driver DIRECTO). Último `enter` sin `exit` = DDI asesino.
- `APCTRACE` en `ntoskrnl/ps/kill.c` (`PspExitThread` enter + preassert
  `kill.c:841`): nonzero al entrar = leak de vida; solo antes = teardown.
- `VADCLEAN/PDETBL/safety net` en `ntoskrnl/mm/ARM3/procsup.c`: ver skill teardown.
- `DISPRESTORE`, `GAMING-PERF`, `GAMING-MOUSE`: ver skills display/perf.
- Regla: degradar a `DPRINT` tras cazar; `ERR` solo si raro+valioso.
  Verificado por `strings`: ausentes = silencioso OK.

## 4. Desensamblado (casos resueltos, reutilizar patrón)

- `Eip=80569139` ×475 → `fxsave (%eax)` en `Ke386SaveFpuState` = bug TS (#1).
- `nv4_disp+0x2116DF` `cmp (%edi),%eax`, `edi=[param1+0x338]=0x9D009C00` =
  puntero rancio tras mode-switch.
- `nv4_disp+0x210aa6/0x210ae8/0x2102ef` leyendo `0x9D00DBA4/0x3E7BBE53`/pool =
  free-proc/UAF teardown.
- `nvoglnt+0x875aa7` lee `0x9A0` en `wglMakeCurrent` = teardown GL.
- `0x805B41B8` = `int 0x2D; int3` = `DebugService` sin debugger (ASSERT con
  texto previo, o `DbgBreakPoint` pelado sin texto).
- Método: DateStamp del log ↔ binario desplegado; desensamblar el `.exe/.sys`
  exacto del target, no el del árbol si hay duda de frankenkernel.

## 5. Cómo leer un caso nuevo

1. Verificar `GAMING-BUILD` con fecha de hoy. Si no, parar y pedir redespliegue.
2. Últimas 100 líneas antes del paro: heartbeats (¿viven?), tormentas
   (¿FPUTRAP/GetThreadConsoleDesktop?), último DDITRACE enter, VAD 64 MB,
   `DxDdDynamicModeChange`, `Failed to restore`, `stale handle`, `free proc`.
3. Foto BSOD: código + 4 parámetros + módulo+offset + EIP. Cruzar con
   desensamblado.
4. Si `kdb:>` con stack automático: es oro (aunque RX muerto impida `bt` manual).
5. No mezclar sesiones: `halo.exe` vs GTA moddeado vs Qt spew se distinguen por
   líneas (`SilentPatchSA.asi`, `QOpenGL*`, `ApiSetResolveToHost`).

## 6. Errores típicos

- Diagnosticar sobre frankenkernel (log rancio + binario nuevo).
- Confundir PID ejecutor (System) con víctima (Halo) en teardown.
- Dereferenciar `Peb`/user-VA en contexto foreign (fault `0x7FFD4094`).
- Imprimir en caliente a 115200 y crear el cuelgue que se mide.
- Culpar a GCC/silicio antes de probar que nadie llama al código (KDNET).
