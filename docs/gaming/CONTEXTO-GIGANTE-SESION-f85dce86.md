# CONTEXTO GIGANTE — Sesión `ses_f85dce86bffezWklgFT5uPfoZw` → Base ReactOS Gaming (Halo + GT 710)

> Origen: sesión opencode titulada **"Instalar RosBE-Unix para compilar ReactOS"**
> (`ses_f85dce86bffezWklgFT5uPfoZw`), 1693 mensajes / 6594 parts / ~2.4 MB de texto.
> Rango temporal: 2026-09-07 → 2026-09-12.
> Este documento es el contexto canónico para continuar el trabajo en esta sesión
> y la fuente de verdad de las 6 skills bajo `.opencode/skills/reactos-*`.
> Complementa (no sustituye) a `GAMING-CHANGES.md` (#1–#31) y al commit
> `7846249175b [GAMING] Halo GT710 base` (58 ficheros, +2197/−99).

---

## 0. Ficha rápida

- **Objetivo de la sesión:** base ReactOS capaz de ejecutar juegos 3D con driver
  NVIDIA real (GT 710, ForceWare 353.62). Hito: **Halo campaña jugable en 3D**,
  salida limpia, resolución restaurada, luego rendimiento y ratón.
- **Máquina target (ReactOS real):** BIOSTAR H81MHV3, i3-4170, **GT 710**,
  monoprocesador (uniprocessor), Realtek `PCI\VEN_10EC&DEV_8136`,
  escritorio 1366x768, juego 800x600. RAM vista según build: 2048 MB / 3562 MB.
- **Máquina dev:** laptop Windows (`C:\Users\garci\...`) + **Kali Rolling en WSL2**
  (`Linux Gustavo 6.18.33.2-microsoft-standard-WSL2`). Trabajo de compilación
  en Linux (`/home/demox/...`), despliegue por red/USB al target.
- **Red:** host WinDbg `192.168.18.156`, target `192.168.18.170`.
- **Driver:** NVIDIA 353.62 (`nv4_mini.sys` 11152080 bytes + `nv4_disp.dll`
  3513984 bytes). 174 imports, todos resuelven, sin stubs. Carga limpia vía
  `devmgmt.msc`. OpenGL rinde. `GL_VENDOR "NVIDIA Corporation"`,
  `GL_VERSION "4.4.0 NVIDIA 353.62"`, `GL_RENDERER "unknown board/PCIe/SSE2"`.
- **Juegos usados:** Halo (principal, `halo.exe`/`haloce.exe`, PID ~840),
  GTA San Andreas (secundario, contaminado por mods — SilentPatchSA.asi,
  APISETs modernos — se aparcó), `glteardown.exe` (reproductor mínimo propio),
  app Qt 5.6.3 (contaminó un log con spew OpenGL, se descartó).
- **Estado final verificado (2026-09-11/12):** Halo fullscreen se juega, se sale,
  vuelve a 1366x768, la sesión sobrevive; ~30 fps con `csmt=0`; ~60 fps en
  escenas ligeras con ajustes bajos. Ver hashes en §7.

---

## 1. Árboles, rutas y convenciones

| Símbolo | Árbol | Ruta dev | Uso |
|---|---|---|---|
| `LH` | `Longhorn-ReactOS-Stable` (Linux) | `/home/demox/Longhorn-ReactOS-Stable`, build `/home/demox/build-LH-Stable` | Base gaming real. HEAD `328deb671c`. Aquí viven casi todos los fixes #1–#31 |
| `WDDM` | `reactos-Wddm-Win32k-Branch` (Windows) | `/mnt/c/Users/garci/reactos-Wddm-Win32k-Branch`, build `output-MinGW-i386` | Rama con soporte WDDM/Win7+. Se compiló al inicio, se portó KSE desde `longhorn-nvidia-uacpi-kshim`, perfil español se fijó aquí |
| `KSE-src` | `longhorn-nvidia-uacpi-kshim` | referencia | Donante del KSE portado |

**Configuración de build LH:**
`configure.sh -DENABLE_ROSTESTS=1 -DENABLE_ROSAPPS=1`, luego `ninja -j12 bootcd`
→ `bootcd.iso` ~504 MB.

**Estados de cada cambio (obligatorio en `GAMING-CHANGES.md`):**
- `[PERMANENT]` → va a la publicación (fix real).
- `[TEMP-DEBUG]` → revertir antes de publicar (instrumentación).
- `[TOLERANCE]` → frontera ante driver cerrado (defensa, no fix del driver).
- `[PARKED]` → aparcado con motivo (ej. fallback servidor usersrv, KDNET).
- `[TEMP-HARDEN]` → endurecimiento temporal (#30 PDEV SEH).

**Registro obligatorio:** todo fix lleva entrada en
`/home/demox/Longhorn-ReactOS-Stable/GAMING-CHANGES.md` y, si es kernel,
marker `GAMING-BUILD __DATE__ __TIME__` en `ntoskrnl/ke/hbd.c:58`.

---

## 2. Cronología completa por fases

### Fase 0 — RosBE-Unix 2.2.1 en Kali (inicio literal de la sesión)
1. Usuario pide instalar RosBE-Unix 2.2.1 (texto del PPA `ppa:reactos/rosbe-unix`).
2. PPA inútil en Kali Rolling → **instalación manual**:
   scripts, `RosBE-Version`, `amd64/version.sh`, symlink `bin/ninja`.
3. Verificación: `i686/x86_64-w64-mingw32-gcc 8.4.0`, `configure.sh` OK.
4. Rutas: `/home/demox/RosBE` → `/home/demox/RosBE-CI`.

### Fase 1 — Copia y compilación WDDM + fixes GCC 8.4
1. Copia `reactos-Wddm-Win32k-Branch` a Linux, `configure.sh
   -DENABLE_ROSTESTS=1 -DENABLE_ROSAPPS=1`.
2. Pregunta "¿qué características potencian esta rama?" → respuesta: vía WDDM,
   driver GPU Win7+, D3D moderno (frente a XPDM/XP).
3. Cascada de errores de compilación Windows (todos con `C:\RosBE\i386\bin\gcc.exe`):
   - `sdk/lib/drivers/rddm/displib/Dxgk.c` (NTAPI/coma).
   - `drivers/crypto/ksecdd/ksecdd.sys` (`BCryptDestroyHash@4` no exportado,
     handles PVOID).
   - `dll/directx/wine/dwrite/freetype.c` (`C_ASSERT` + `ARRAYSIZE`→`sizeof`).
   - `modules/rostests/winetests/d2d1` (wrappers WIDL, vtbls, imports).
   - `modules/rostests/winetests/dwrite` (typo `-D-DWINETEST...`, vtbls,
     sequence-point).
   - `modules/rostests/winetests/ntdll` (psapi/spec exports).
   - `win32ss/dwm/milcore/core/av/av.h` (`Vmr9.h`→`vmr9.h` mayúsculas).
   - `sdk/lib/3rdparty`, `sdk/lib/dxsdk`, `sdk/include` varios.
4. Metodología: pegar log `FAILED: ...`, agente localiza fichero, edita mínimo,
   recompila. Todos `[PERMANENT]`.

### Fase 2 — Bug perfil español
- Síntoma: instalación en español falla al reiniciar creando perfiles, pide
  reiniciar; en inglés no pasa. Log aportado (~20K).
- Causa: `es-ES.rc` sin `IDS_LOCALLOWAPPDATA`/`IDS_ROAMINGAPPDATA` +
  `CreateStandardProfile` no creaba directorios intermedios.
- Fix (árbol WDDM): `dll/win32/userenv/lang/es-ES.rc` (+2 líneas) +
  `dll/win32/userenv/setup.c` (+22, crear intermedios). `[PERMANENT]`.

### Fase 3 — KSE + MediaFoundation + ISO base
- Port de **KSE** desde `longhorn-nvidia-uacpi-kshim`:
  `ntoskrnl/kse/{kse.c,driverscope.c,versionlie.c}` (~881 líneas),
  `ntoskrnl/include/internal/kse.h` (154), `media/sdb/drvmain.xml` (+54) +
  `CMakeLists.txt`, hooks en `ntoskrnl.h`, `io/iomgr/driver.c`,
  `io/iomgr/iomgr.c`, `mm/ARM3/sysldr.c`, `ntos.cmake`, `sdk/include/ndk/iotypes.h`.
- Efecto: driver 353.62 carga, OpenGL renderiza, resolución sube.
- Fixes MediaFoundation/ffmpeg/winedmo/colorcnv/wmvdecod → `bootcd.iso` 504 MB.

### Fase 4 — KDNET investigado y aparcado (lección de honestidad)
- Objetivo: depuración viva por red (Realtek 8136) con WinDbg host.
- Pasos: `freeldr.ini` oculto (`attrib -s -h`), entrada `ReactOS (KDNET)`,
  `kdnet.dll` presente, firewall (`New-NetFirewallRule UDP 50000`, ICMP),
  `netstat -ano | findstr 50000` (PID 3032 escuchando), Wireshark con filtro
  `udp.port==50000 or bootp` cubriendo el arranque → **cero paquetes**.
- Veredicto con evidencia de código: FreeLDR **carga** `kdnet.dll`
  (regla `kd<PUERTO>.dll` en `winldr.c`) pero **nadie ejecuta su punto de
  entrada** (`KdDebuggerInitialize0` del transport nunca invocado; kernel solo
  conoce Serial/Screen/File; `KdSendPacket` ligado al serie). El éxito de 2022
  necesitó piezas no publicadas. **PARKED**, no por pereza. Reglas firewall
  revertidas (`Remove-NetFirewallRule`, `netsh delete rule`).

### Fase 5 — Serie real por fin (usbipd) + captura viva
- Compras: PCI serie para el target (sin COM), adaptador USB-DB9 para la laptop,
  **cable null-modem hembra-hembra completo** (cruce 2↔3 + handshake 7↔8).
  Lección: género ≠ cruce; adaptador recto = TX contra TX = silencio.
  Excepción: cable vendido como "USB to null modem".
- Topología: `[ReactOS COM1] —null-modem— [USB-DB9] → laptop`.
- WSL2: `winget install dorssel.usbipd-win`, `usbipd list/bind/attach --wsl`,
  `/dev/ttyUSB0` 115200 `cs8 -cstopb -parenb -ixon raw`, `chmod 666`.
- Captura: `/home/demox/serial-caps/cap1.log` (`cat`/`minicom`/`picocom`).
- **Asimetría descubierta:** TX target→WSL OK, **RX WSL→target muerto**
  (KDB sordo a `b/bt/cont`, Ctrl+C/BREAK ignorados, XMODEM sin ACK; dos stacks
  — kdserial y serial.sys — sordos = hardware: socket DB9 del target).
  Consecuencia: forense solo con lo que el target imprime solo + fotos BSOD.
- `kdbwatch.sh`: vigilante que ante prompt KDB inyecta `b`→`bt`→`cont`
  (requiere TX viva; aquí solo observaba).

### Fase 6 — Forense FPU + csrss (los dos primeros cuelgues explicados)
- `glteardown.exe` reproduce crash en `wglMakeCurrent` (`nvoglnt+0x875aa7`
  lee `0x9A0`) + `STOP 0x1E (0x80000003)` en `DebugService int 0x2D; int3`.
- Traza Halo→dxdiagn→wbemprox(WMI)→ntdll→`int 0x2D`→`KeTrapFrameToContext`+flush
  FPU→pagefault→BSOD.
- **FPUTRAP** (`ntoskrnl/ke/i386/traphdlr.c:739`, `KiTrap07Handler`):
  instrumentación `Eip/Irql/CR0/Thread/NpxThread/NpxState`; v2 `KeBugCheckEx
  0x46505554 ("FPUT")` si IRQL>DISPATCH; v3 traza NpxThread.
- **HBD heartbeat** (`ntoskrnl/ke/hbd.c:39`, DPC cada ~2 s,
  `HEARTBEAT: tick=N Thread=... Irql=...`): distingue kernel-muerto vs
  display-muerto. + marker `GAMING-BUILD __DATE__ __TIME__` (`hbd.c:58`).
- **NpxThread cleanup** (`ntoskrnl/ps/kill.c`, `PspExitThread`): si el hilo
  saliente aún es dueño FPU del PRCB, soltar (`NpxThread=NULL`,
  `NpxState=NOT_LOADED`).
- **Cuelgue csrss 99%:** Halo lanza 8600 `GetThreadDesktop` en ráfaga (+resto
  hasta 10056); cada fallo `0xc0000008` imprimía `DPRINT1` a 115200 (~10 ms) →
  minutos de E/S bloqueante. Fix: degradar a `DPRINT` y luego eliminar
  (`winsrv_SILENT.dll`). Error intermedio famoso: se dio `csrss.exe` cuando el
  código vive en `winsrv.dll` — se corrigió.
- **Fix raíz FPU (#1):** `Eip=80569139` ×475 = `fxsave (%eax)` del propio kernel
  en `Ke386SaveFpuState`. El handler reanudaba con TS puesto
  (`Cr0 |= SaveArea->Cr0NpxState` + retorno condicional) → re-faulteo inmediato,
  bucle #NM en kernel, doble falta en usuario, livelock con 3 hilos + "is hung".
  Fix: `Cr0 &= ~CR0_TS; Cr0 |= (Cr0NpxState & ~TS)` + retorno incondicional
  (`KiEoiHelper`). `FPUTRAP DPRINT1→DPRINT`.

### Fase 7 — Halo 3D jugable + teardown VRAM (la semana del MM)
- Con FPUFIX + SILENT: **Halo campaña en 3D a 3-5 fps** (suficiente como hito).
- Crash salida #1: `DRIVEROBJ_vCleanup` → `nv4_disp+0x210ae8` lee heap liberado.
- Crash mode-switch: `STOP 0x50 nv4_disp+0x2116DF cmp (%edi),%eax`,
  `edi=[param1+0x338]=0x9D009C00` no mapeado, tras `DxDdDynamicModeChange`.
- Instrumentación: `DDITRACE enter/exit` en `DrvEnableSurface/AssertMode/
  DisableSurface/BitBlt/CopyBits/DescribePixelFormat/SetPixelFormat/SwapBuffers`
  (`pdevobj.c, bitblt.c, copybits.c, wingl.c`) + `MapMemory` en `dxg/ddsurf.c`
  (vía `DbgPrint`: dxg llama al driver DIRECTO, sin frame win32k).
- **#3 MDL boundary** (`mm/ARM3/mdlsup.c`, `MiUnmapLockedPagesInUserSpace`):
  solo `MiDeletePde` en borde PDE o página única; si no, omitir (fuga segura
  vs UAF). Cazado por `ASSERT MiIsPteOnPdeBoundary` en vivo.
- **#19 VRAM drain** (`mm/ARM3/procsup.c`, `MmCleanProcessAddressSpace`):
  rama `VadDevicePhysicalMemory` borra PTEs 4K directas (sin contabilidad PFN).
  Bug propio intermedio: faltaba `MiDecrementShareCount` por PTE → assert
  `ShareCount==1` (`virtual.c:511`) en VAD 64 MB `[14330000-1832FFFF]` type=1.
  Fix: decrementar como `MiDeletePteRange`/mdlsup.
- **Safety net + PDETBL + VADCLEAN** (`MmDeleteProcessAddressSpace`):
  attach-aware al PID víctima (no ejecutor — System limpiando a Halo mentía);
  `PDETBL` vuelca PDEs válidas; safety net libera tablas vacías
  (`MiDeletePde` si 0 PTEs válidas — el path genérico solo liberaba por
  dec→0, leak silencioso en release, assert en debug) + prototipos.
  Lección: `PsGetCurrentProcess()==Process` es test WRONG; correcto es
  ejecutor==víctima o attached-TO-víctima. Nunca dereferenciar `Peb` (fault
  `0x7FFD4094` visto). Orden de locks: PFN dentro de WS, prints antes de PFN.
  #24 higienizó a `DPRINT` (~150 líneas/salida → 0).
- **#6 DRIVEROBJ SEH** (`gdi/eng/driverobj.c`, `DRIVEROBJ_vCleanup`):
  `_SEH2_TRY/EXCEPT` + save/restore **ULONG** `CombinedApcDisable` + TID.
  v1 forzaba a 0 y rompía invariante (`resource.c:1863`); v2 guarda/restaura;
  v2b SHORT→ULONG (campo son DOS contadores; SHORT truncaba guarded retenido
  `0xFFFF0000`). Log `calling free proc %p obj %p dhpdev %p hdev %p`.
- **#11 stale-handle** (`gdi/eng/device.c`, `EngDeviceIoControl`): valida
  `MmIsAddressValid+Type+StackSize` (NO `Size`: `IoCreateDevice` suma extensión,
  mataba arranque con `0xB4`; bugcheck `0x35` con `StackSize` 0). SEH no traga
  bugchecks; el chequeo sí.
- **#14 orden GDI** (`gdi/ntgdi/gdiobj.c`, `GDI_CleanupForProcess`): **dos
  pasadas, DRIVEROBJs PRIMERO**. Causa raíz de `0x50` de salida: callbacks del
  driver leían hermanos ya liberados (sesión). Faults a dirección kernel →
  `MmAccessFault` hace bugcheck directo SIN dispatch: ningún SEH los traga,
  solo el orden los evita.
- **#12 backoff Video1** (`EngpUpdateGraphicsDeviceList`): 60 s a devices con
  CREATE denegado `0x22` (2º head; Afterburner lo sondea en hot-loop → miles
  de líneas + CPU).
- **#9 APCTRACE** (`PspExitThread`): `CombinedApcDisable` al entrar y antes del
  assert `kill.c:841` (nonzero al entrar = leak de vida; solo antes = teardown).
- **#17 GDIDESTROY** (`gdi/ntgdi/init.c`, `GdiProcessDestroy`): desmapea
  `GdiSharedHandleTable` + log con dirección (candidata a tabla zombie idx498).

### Fase 8 — Restore de resolución (#20/#22/#23, trilogía del 800x600)
- Síntoma: al salir de Halo (800x600) el escritorio quedaba clavado en 800x600 +
  `Failed to restore display mode` (`display.c:947`).
- #20: `UserDisplayNotifyShutdown(NULL)` leía registro sin `X/YResolution` →
  `BADMODE`. Fix: guardar modo escritorio al primer `CDS_FULLSCREEN`, usarlo si
  registro vacío.
- #22: el record coincidía pero el hardware divergía vía DXG (`matches → skip`
  nunca re-afirmaba). Fix: restore (`pdm==NULL`) NO salta aunque coincida:
  fuerza `PDEVOBJ_bSwitchMode` (re-assert; éxito limpia `gpFullscreen`) +
  forense `DISPRESTORE`.
- #23 (causa real): el guardado leía `pdmwDev` DESPUÉS de que el juego ya lo
  cambiara (plain switch previo al primer FULLSCREEN) → guardaba 800x600.
  Fix: `UserSeedDesktopMode` siembra baseline desde el probe de defaults del
  driver (1366x768, estático) en `MDEVOBJ_Create(!pdm)`; FULLSCREEN solo guarda
  si no hay baseline; plain NO invalida; solo `CDS_UPDATEREGISTRY` exitoso
  resetea. `[PERMANENT]`.

### Fase 9 — Rendimiento 3-5 fps → 30 fps → 60 fps (#25–#28)
- #25 medición: `d3dwine` fps-meter (presents+draws). Present inocente
  (<1 ms/frame vía GL). Draws ~400/frame a ~0.5 ms con CSMT on.
- Descartados: serie (~1 línea/2 s en estable), VRAM=0 (fallback a 1280 MB de
  la DB no mueve fps), GPU (`GL 4.4 NVIDIA`, `card GTX470 10de:06cd`,
  HW vertex `software 0x0`).
- #26 `HKCU\Software\Wine\Direct3D` DWORD `csmt=0` (single-threaded CS):
  **5 fps → ~30 fps** estables en i3 monoprocesador. Handoff entre hilos por
  draw era el impuesto. DEFAULT GAMING, documentado, no binario.
- #28 hito 60 fps: `csmt=0` + ajustes bajos, 120 frames en ~2010 ms = 59.7 fps
  (vsync 60 Hz; present absorbe espera 400-900 ms/120). Ley:
  `fps ~= 1/max(draws*80us, 16.7ms)`: ~200 draws→60, ~400→30, ~1000→20.
  Frame clavado en ~33.5 ms aunque draws varíen 335↔477 = espera fija
  (vsync a 60 Hz cayendo cada 2 vblanks o cap 30 fps del Halo).
  Pendiente 30→60 en pesadas: bajar coste/draw (build `-O0`).
- Instrumentación: `GL_RENDERER/VENDOR/VERSION` + `card ... vidmem ... emulated
  vram` + fallback VRAM (`adapter_gl.c`), `CreateDevice SOFTWARE/HARDWARE/MIXED`
  (`d3d9/directx.c`), draws (`cs.c`→`swapchain.c` fps-meter cada 120 presents
  con split gdi/gl + ms present).

### Fase 10 — Ratón FPS (#29–#31, abierto)
- #29 dinput (`dinput/mouse.c`, `warp_check`): solo recentraba UNA vez por
  acquire (`if (!clipped)`); luego el cursor vagaba hasta clavarse en borde y
  los deltas morían (ventana y completa). Fix: recentrar siempre en cada poll
  exclusivo (throttle 10 ms) + mantener clip. Marker `GAMING-MOUSE exclusive
  acquire`. `[PERMANENT]` (mecánica FPS estándar).
- Descubrimiento: **Halo nunca adquiere el mouse por dinput** (teclado sí) →
  usa ClipCursor + posiciones absolutas sin recentrar. Traza `GAMING-MOUSE` en
  `UserClipCursor/UserSetCursorPos` (win32k): clip `(43,24)-(1323,744)` repetido
  + `SetCursorPos hook 1` = movimientos físicos, no recentrado del juego.
  Pendiente: recentrado asistido o `MouseWarpOverride=force`.
- #30 contención faults driver en switches (`pdevobj.c`): `0x50` en
  `nv4+0x2102ef` leyendo pool liberado tras doble switch (UAF del driver).
  `PDEVOBJ_bSwitchMode` partido en `Unsafe` + wrapper SEH con restore
  best-effort, semáforos siempre liberados, APC preservado. `[TEMP-HARDEN]`.

---

## 3. Cómo construíamos cada fix (metodología canónica)

1. **Síntoma con evidencia:** foto BSOD (código + `nv4_disp+offset` + dirección
   que lee) + cola del `cap1.log` (20–100 líneas previas). Sin marker
   `GAMING-BUILD` con fecha de hoy, el arranque no cuenta (frankenkernel).
2. **Hipótesis con nombre de fichero:línea:** ej. `traphdlr.c:739`,
   `mdlsup.c:74`, `virtual.c:511`, `resource.c:1863`, `display.c:947`.
3. **Instrumentación TEMP-DEBUG mínima y marcada:** `FPUTRAP`, `DDITRACE`,
   `APCTRACE`, `VADCLEAN`, `PDETBL`, `DISPRESTORE`, `GAMING-PERF`,
   `GAMING-MOUSE`, `GAMING-BUILD`. Siempre reversible a `DPRINT`/`ERR` barato.
4. **Desensamblado del binario del target:** `Eip=80569139` → `fxsave (%eax)`
   en `Ke386SaveFpuState`; `nv4+0x2116DF` → `cmp (%edi),%eax` con
   `edi=[param1+0x338]`; `0x805B41B8` → `int 0x2D; int3` (DebugService sin
   debugger). DateStamp del log ↔ binario para resolver símbolos.
5. **Fix mínimo con comentario que cita el porqué:** guardar/restaurar en vez de
   forzar; validar en vez de atrapar bugcheck; ordenar en vez de blindar SEH
   donde el fault es a kernel.
6. **Build con verificación por contenido, no por fecha:** `strings` busca
   markers (`safety net`, `GAMING-BUILD`, `attached to victim`,
   `dropped device-physical`, `APCTRACE`); `sha256sum`; tamaño
   (ej. `dinput.dll` 274944). Reloj WSL invierte mtimes → `touch` ante duda,
   ninja salta recompilas en silencio.
7. **Despliegue por plan A:** `py -3 -m http.server 8080 -d C:\Users\garci` +
   `dwnl http://192.168.18.156:8080/<fichero> C:\fx\...` + swap offline +
   reboot. Kernels a `C:\` + entrada freeldr clonada; DLLs a `system32`;
   `dxg.sys` a `drivers`. Ver §7.
8. **Confirmación en log:** ausencia/presencia de la señal esperada
   (`cero GetThreadConsoleDesktop` = SILENT activo; `dropped device-physical
   (N erased)` + `PDETBL` + `safety net swept` = VRAM OK;
   `restore candidate 1366x768 vs current 800x600` + re-assert = restore OK;
   `GAMING-PERF fps` = perf OK).
9. **Registro:** entrada en `GAMING-CHANGES.md` con estado + binario que lo lleva.

**Costes que aprendimos a respetar:**
- Cada `DPRINT1` a 115200 ≈ 10 ms síncronos. Una tormenta (10K líneas) = minutos.
- `DPRINT` fuera del binario = 0 coste. `ERR` raro y valioso se queda.
- `ASSERT` en debug = prompt KDB = "cuelgue" si nadie lo atiende.

---

## 4. Catálogo fix por fix (#1–#31)

Ver `GAMING-CHANGES.md` para la tabla canónica. Resumen con binario portador:

| # | Fichero(s) | Cambio esencial | Estado | Binario |
|---|---|---|---|---|
| 1 | `ntoskrnl/ke/i386/traphdlr.c` `KiTrap07Handler` | Reanudar con TS limpio + retorno incondicional | PERMANENT | `ntoskrnl_APCDBG.exe` |
| 2 | mismo | `FPUTRAP DPRINT1→DPRINT` | TEMP-DEBUG | mismo |
| 3 | `ntoskrnl/mm/ARM3/mdlsup.c` | `MiDeletePde` solo en boundary | PERMANENT | mismo |
| 4 | `winsrv/usersrv/init.c` | print eliminado | TEMP-DEBUG (fallback PARKED) | `winsrv_SILENT.dll` |
| 5 | `user32/misc/desktop.c` | caller-ID rate-limited 8/proceso + rama SERVERFAIL | TEMP-DEBUG revertir | `user32_CALLER*.dll` (no desplegar) |
| 6 | `gdi/eng/driverobj.c` | SEH + save/restore ULONG + TID | TOLERANCE | `win32k_CLEANUP.sys` |
| 7 | `ntoskrnl/ke/hbd.c` | heartbeat DPC 2 s + marker GAMING-BUILD | TEMP-DEBUG (marker PERMANENT) | `ntoskrnl_APCDBG.exe` |
| 8 | `ntoskrnl/ps/kill.c` | limpia NpxThread saliente | PERMANENT | mismo |
| 9 | mismo | APCTRACE enter/preassert | TEMP-DEBUG revertir | mismo |
| 10 | `gdi/eng/pdevobj.c,bitblt.c,copybits.c,ntgdi/wingl.c` | DDITRACE enter/exit DDIs | TEMP-DEBUG revertir | `win32k_CLEANUP.sys` |
| 11 | `gdi/eng/device.c` `EngDeviceIoControl` | valida handle (Type+StackSize, sin Size) | TOLERANCE | mismo |
| 12 | mismo `EngpUpdateGraphicsDeviceList` | backoff 60 s Video1 0x22 | PERMANENT | mismo |
| 13 | `mm/ARM3/procsup.c` | safety net tablas vacías | PERMANENT | `ntoskrnl_APCDBG.exe` |
| 14 | `gdi/ntgdi/gdiobj.c` | DRIVEROBJs primero (dos pasadas) | PERMANENT | `win32k_CLEANUP.sys` |
| 15 | `reactx/dxg/ddsurf.c` | DDITRACE MapMemory vía DbgPrint | TEMP-DEBUG | `dxg_TRACE.sys` |
| 16 | `mm/ARM3/procsup.c` | PDETBL+VADCLEAN+safety net con PID víctima | TEMP-DEBUG | `ntoskrnl_APCDBG.exe` |
| 17 | `gdi/ntgdi/init.c` | desmapea GdiSharedHandleTable + GDIDESTROY | PERMANENT si confirma | `win32k_CLEANUP.sys` |
| 18 | `ntoskrnl/ke/hbd.c` | GAMING-BUILD marker | PERMANENT | `ntoskrnl_APCDBG.exe` |
| 19 | `mm/ARM3/procsup.c` | ShareCount por PTE drenada VRAM | TEMP-DEBUG hasta salida limpia | mismo |
| 20 | `ntuser/display.c` | guarda modo escritorio en FULLSCREEN, fallback si registro vacío | PERMANENT | `win32k_CLEANUP.sys` |
| 21 | `gdi/eng/driverobj.c/device.c` | forense free proc/obj/dhpdev/hdev + caller reject | TEMP-DEBUG | mismo |
| 22 | `ntuser/display.c` | restore re-assert aunque coincida + DISPRESTORE | log TEMP, re-assert PERMANENT | mismo |
| 23 | `ntuser/display.c/mdevobj.c` | baseline desde arranque (`UserSeedDesktopMode`), plain no invalida | PERMANENT | mismo |
| 24 | `procsup.c/kill.c` | manguera a DPRINT, quedan ERR baratos | higiene | `ntoskrnl_APCDBG.exe` |
| 25 | `wined3d/swapchain.c` | fps-meter presents+draws | TEMP-DEBUG/doc | `d3dwine_PERF.dll` |
| 26 | registro | `HKCU\Software\Wine\Direct3D csmt=0 DWORD` | DEFAULT GAMING | — (no binario) |
| 27 | `wined3d/adapter_gl.c` | VRAM fallback a DB (descartado, no mueve fps) | descartado | `d3dwine_PERF.dll` |
| 28 | ajustes juego | 60 fps con ajustes bajos (vsync) | hito | — |
| 29 | `dinput/mouse.c` | recentrar siempre en poll exclusivo | PERMANENT | `dinput_MOUSE.dll` |
| 30 | `gdi/eng/pdevobj.c` | `bSwitchModeUnsafe` + wrapper SEH | TEMP-HARDEN | `win32k_CLEANUP.sys` |
| 31 | `ntuser/cursoricon.c` + `user32` | traza ClipCursor/SetCursorPos | pendiente decidir fix | mismo |

Heredados al inicio: KSE portado, build fixes GCC 8.4, perfil español.

---

## 5. Herramientas de laboratorio (no se publican con el OS)

- `rx.c` / `xmsend.py`: receptor XMODEM-CRC en ReactOS + emisor Python;
  `rx stage` agenda canje vía `PendingFileRenameOperations`. **Plan B en pausa**
  por RX muerto. Tiempos a 115200 (~10 KB/s): DLL ~1 min, kernel ~8 min.
  COM1 es también puerto debug → transferir en entrada no-debug.
- `kdbwatch.sh`: vigilante `b`→`bt`→`cont` ante prompt KDB (requiere TX viva).
- `glteardown.exe` (`/home/demox/gl_teardown_test/glteardown.c`): reproductor
  mínimo del crash `wglMakeCurrent`.
- Servidor HTTP plan A: `py -3 -m http.server 8080 -d C:\Users\garci` +
  `dwnl http://192.168.18.156:8080/... C:\fx\...`. Alternativa WSL:
  `netsh interface portproxy add v4tov4 listenaddress=192.168.18.156
  listenport=8080 connectaddress=172.30.120.155 connectport=8080`.
- `d3dwine_PERF.dll` / `d3d9_PERF.dll` / `dinput_MOUSE.dll`: instrumentación
  perf/mouse por swap offline (verificar tamaño/fecha/sha tras move).

---

## 6. Mapa de despliegue (binario → lleva → destino)

| Binario | Lleva (#) | Destino target |
|---|---|---|
| `ntoskrnl_APCDBG.exe` (+`GAMING-BUILD <fecha>`) | 1,2,7,8,9,13,16,18,19,24 | `C:\` + entrada freeldr clonada |
| `winsrv_SILENT.dll` | 4 | `C:\ReactOS\system32\winsrv.dll` (backup `csrss.exe` restaurado) |
| `win32k_CLEANUP.sys` | 6,10,11,12,14,17,20,21,22,23,30,31 | `C:\ReactOS\system32\win32k.sys` |
| `dxg_TRACE.sys` | 15 | `C:\ReactOS\system32\drivers\dxg.sys` |
| `d3dwine_PERF.dll` → `d3dwine.dll` | 25,27 + fps/draws/VRAM | `C:\ReactOS\system32\d3dwine.dll` |
| `d3d9_PERF.dll` → `d3d9.dll` | CreateDevice flags | `C:\ReactOS\system32\d3d9.dll` |
| `dinput_MOUSE.dll` → `dinput.dll` | 29,31 + lifecycle | `C:\ReactOS\system32\dinput.dll` |
| `user32_CALLER*.dll` | 5 | **NO desplegar** (diagnóstico cerrado: sondeador `nvoglnt 69D9871C`) |

**Hashes finales verificados (ESTADO FINAL FASE ESTABILIDAD 2026-09-11):**
- `ntoskrnl_APCDBG.exe` `569baf363eba68ade7258470f3c593fee6ba4e7d5c57f32d33473ea4f3f44ee6`
- `win32k_CLEANUP.sys` `83e5ff9869719e4c8d71beb1d9247a0b3b72ecd99ee48c8be614e9994c07100e`
  (luego `b0a5ebd2…` con #30 SEH switch + mouse trace; `0870a1e6…` con cursor trace)
- `dxg_TRACE.sys` `daed3a06e459bea8b70a09c12fd3e2bd8b46e821cb660a667c49a3479161cb9c`
- `winsrv_SILENT.dll` `21d10786b30c3df418a63a0780484dffcac1d8711285dcf12a10a9384dffcac`
  (prefijo `21d10786…`)
- Intermedios históricos: `ntoskrnl_FPUFIX.exe 0b213a82…`, `ntoskrnl_MDLFIX.exe`,
  `ntoskrnl_HBD.exe`, `csrss_NOPRINT.exe 26c146a6…` (equivocado),
  `winsrv_SILENT.dll 21d10786…`, `user32_CALLER.dll 80b8e310…`,
  `win32k_CLEANUP.sys e1602cc4…/8a252cba…/6eec0a29…/41d2dad6…`,
  `d3dwine_PERF.dll 1d18c543…/716bb379…/2098680c…/483573e8…/abee3b82…`,
  `d3d9_PERF.dll 6a027e03…`, `dinput_MOUSE.dll 32c12bec…/6a8780a8…/962a5cf0…/
  b50aee72…/e5949007…` (274944 bytes).

**LECCIÓN frankenkernel:** el reloj WSL invierte mtimes y ninja salta recompilas
(binarios rancios con cara de frescos). Verificar por `strings`, no por
hash/fecha. Forzar con `touch` ante duda. Sin `GAMING-BUILD` con fecha de hoy,
el arranque no cuenta.

---

## 7. Logros (verificados en GT 710 + 353.62)

1. RosBE-Unix 2.2.1 manual en Kali + `configure.sh` OK + GCC 8.4.
2. Builds Windows+Linux reparados (WIDL, d2d1/dwrite, ntdll/psapi, ksecdd,
   Dxgk, MediaFoundation/ffmpeg/winedmo, es-ES).
3. KSE portado → driver 353.62 carga, OpenGL rinde, resolución sube.
4. `KiTrap07` TS fix → fin del livelock #NM ("is hung" + display muerto).
5. Spam serie eliminado → fin del csrss 99% (SILENT verificado: cero líneas).
6. Halo campaña 3D jugable (3-5 fps primero, luego 30/60).
7. Salida limpia sin matar sesión (VRAM drain + ShareCount + orden GDI +
   SEH + stale-handle).
8. Resolución restaurada a 1366x768 (baseline arranque + re-assert).
9. Rendimiento: `csmt=0` 5→30 fps; 60 fps en ligeras; ley draws medida.
10. Ratón dinput diagnosticado (Halo usa Win32 absoluto, no dinput mouse).
11. Serie vivo unidireccional + forense sin KDB interactivo.
12. `GAMING-CHANGES.md` #1–#31 + commit gaming + README + placeholders
    `docs/gaming/screenshots|videos`.

---

## 8. Pendientes (lo que queda)

- [ ] Crash salida residual: `nv4_disp` free proc a veces persigue punteros basura
  (nondeterminista, contenido por la guardia). Verificar salida limpia con #6 en
  800x600 (`Failed to restore display mode` residual es bug separado si aparece).
- [ ] `STOP 0x50 nv4_disp+0x2116DF` en mode-switch determinista en stack viejo,
  pendiente reproducir en stack nuevo; #30 lo contiene (reintento/modo anterior).
- [ ] `Video1 0xc0000022` (Afterburner, backoff 60 s) — cosmético.
- [ ] 30→60 en escenas pesadas: bajar coste/draw (build `-O0` actual 2-5×;
    Release son horas — plan B). Mirar ajuste Framerate/VSync del juego
    (`Failed to set swap interval` visto).
- [ ] Ratón Win32 (#31): decidir fix (recentrado asistido en win32k o
    `MouseWarpOverride=force` si el juego tolera dinput). Faltan líneas
    `GAMING-MOUSE ClipCursor/SetCursorPos` de 30 s moviendo ratón.
- [ ] Serie RX muerta (socket DB9 target). Plan: otra PC / rewire TX→RX.
    Plan B XMODEM en pausa hasta entonces.
- [ ] Antes de publicar: revertir todo `[TEMP-DEBUG]` (2 parcial, 5, 7, 9, 10,
    15, 16, 21, parte de 22, forense perf/mouse) y decidir #4 (restaurar print o
    dejar fallback) y #19/#30 (endurecer o mantener).
- [ ] Confirmar `PDETBL pid=4 idx0+idx498` víctima real (apagado vs salida Halo).
- [ ] Confirmar #17 tabla zombie por dirección GDIDESTROY.

---

## 9. Glosario de señales del log (qué significa cada una)

- `GAMING-BUILD <fecha hora>` → kernel gaming vigente. Sin esto, log rancio.
- `HEARTBEAT: tick=N Thread=... Irql=...` → kernel vivo. Siguen = display/input
  muertos, no kernel. Se paran = wedge total.
- `FPUTRAP: Eip=... NpxThread=...` → tormenta #NM (cara con TS fix).
- `GetThreadConsoleDesktop(...) failed 0xc0000008` → spam viejo (SILENT lo elimina).
  `caller=69D9871C desk=144` = nvoglnt sondeando (6.5K, éxito siempre).
- `dropped device-physical ... (N erased)` → VRAM drenada OK.
- `PDETBL pid=<victima> idx=...` → tablas restantes. ¡PID víctima, no ejecutor!
- `safety net ... swept` → tablas huérfanas liberadas.
- `APCTRACE enter/preassert apc=N` → caza leak APC (vida vs teardown).
- `DDITRACE enter X sin exit` → DDI donde muere nv4 (ej. `+0x2116DF`).
- `DRIVEROBJ_vCleanup: calling free proc ...` + `stale ... rejected (caller ...)`
  → orden causa/efecto salida.
- `DISPRESTORE seeded/saved/candidate/re-asserting` → trilogía restore.
- `GAMING-PERF fps: 120 frames in ... present ... draws ...` → perf.
- `GAMING-MOUSE ...` → ratón (acquire/load/ClipCursor/SetCursorPos).
- `Could not open device \Device\Video1, 0xc0000022` → 2º head (backoff).
- `fixme wined3d ... GL_FRAMEBUFFER_UNSUPPORTED` por frame → FBO sin soporte
  (serie lo amplificaba; no es la causa con serie apagado).
- `*** Assertion failed` antes de `int 0x2D; int3` → ASSERT (con fichero:línea);
  sin texto previo → `DbgBreakPoint` pelado.

---

## 10. Cómo continuar en esta sesión (protocolo)

1. Pedir al usuario: binarios desplegados (sha/tamaño) + entrada freeldr usada +
   `GAMING-BUILD` visible + cola del log (no el fichero entero si es grande).
2. No diagnosticar sobre frankenkernel. Checklist cerrado §3 paso 1.
3. Un fix por iteración, con entrada `GAMING-CHANGES.md` y estado.
4. Verificar por `strings` + sha, no por fecha.
5. Mantener serie a 115200, captura corriendo, watcher solo como observador
   (RX muerto).
6. Despliegue solo por plan A salvo orden expresa.
7. Ante `kdb:>` sin TX: ciclo de potencia manual, no insistir por serie.

*Fin del contexto gigante. Fuente: sesión `ses_f85dce86bffezWklgFT5uPfoZw`
completa + `GAMING-CHANGES.md` + commit `7846249175b` + diffs verificados.*
