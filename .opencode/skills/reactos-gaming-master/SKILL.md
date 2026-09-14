---
name: reactos-gaming-master
description: Use when working on ReactOS gaming base (Halo, GT 710, NVIDIA 353.62, GAMING-CHANGES, win32k, ntoskrnl). Master context with trees, deploy map, fix states and frankenkernel lessons.
---

# ReactOS Gaming Master — contexto maestro de la base jugable

Fuente canónica extendida: `docs/gaming/CONTEXTO-GIGANTE-SESION-f85dce86.md`
+ `GAMING-CHANGES.md` (#1–#31) + commit `7846249175b`.

## 1. Qué es esto

Base ReactOS que ejecuta **Halo campaña en 3D** con driver NVIDIA real
(GT 710, ForceWare 353.62), sale limpio, restaura 1366x768 y sobrevive.
Target: BIOSTAR H81MHV3, i3-4170, monoprocesador, Realtek 8136.
Dev: Windows `C:\Users\garci\...` + Kali WSL2 `/home/demox/...`.

## 2. Árboles

- `LH` = `Longhorn-ReactOS-Stable` en `/home/demox/Longhorn-ReactOS-Stable`
  (HEAD `328deb671c`), build `/home/demox/build-LH-Stable`.
  Aquí viven los fixes #1–#31. Este repo (`reactos-gaming`, rama `main`,
  remoto `WallUG/reactos-gaming`) es su publicación.
- `WDDM` = `reactos-Wddm-Win32k-Branch` en
  `/mnt/c/Users/garci/reactos-Wddm-Win32k-Branch`, build `output-MinGW-i386`.
- Build LH: `configure.sh -DENABLE_ROSTESTS=1 -DENABLE_ROSAPPS=1` +
  `ninja -j12 bootcd` → `bootcd.iso` ~504 MB.

## 3. Estados obligatorios (cada cambio los lleva)

- `[PERMANENT]` publicar. `[TEMP-DEBUG]` revertir antes de publicar.
- `[TOLERANCE]` frontera ante driver cerrado. `[PARKED]` aparcado con motivo.
- `[TEMP-HARDEN]` endurecimiento temporal (#30).
- Todo fix → entrada en `GAMING-CHANGES.md`. Todo kernel → marker
  `GAMING-BUILD __DATE__ __TIME__` en `ntoskrnl/ke/hbd.c:58`.

## 4. Mapa de despliegue + hashes vigentes

| Binario | Destino |
|---|---|
| `ntoskrnl_APCDBG.exe` | `C:\` + entrada freeldr clonada |
| `winsrv_SILENT.dll` | `C:\ReactOS\system32\winsrv.dll` |
| `win32k_CLEANUP.sys` | `C:\ReactOS\system32\win32k.sys` |
| `dxg_TRACE.sys` | `C:\ReactOS\system32\drivers\dxg.sys` |
| `d3dwine_PERF.dll`→`d3dwine.dll`, `d3d9_PERF.dll`→`d3d9.dll`, `dinput_MOUSE.dll`→`dinput.dll` | `system32` por swap offline |
| `user32_CALLER*.dll` | NO desplegar (diagnóstico cerrado) |

Hashes fase estabilidad: ntoskrnl `569baf36…`, win32k `83e5ff98…`
(luego `b0a5ebd2…` con #30), dxg `daed3a06…`, winsrv `21d10786…`.
Ver fichero de contexto §6 para la lista completa e intermedios.

## 5. Lecciones que no se negocian

1. **Frankenkernel:** reloj WSL invierte mtimes, ninja salta recompilas.
   Verificar por `strings` (markers), no por fecha/hash. `touch` ante duda.
   Sin `GAMING-BUILD` con fecha de hoy, el arranque no cuenta.
2. **Serie cuesta:** cada `DPRINT1` a 115200 ≈ 10 ms. Tormenta de 10K = minutos.
   `DPRINT` = 0 coste. `ERR` solo si raro y valioso.
3. **RX muerto:** TX target→WSL OK, RX WSL→target muerto (socket DB9 target).
   KDB sordo, XMODEM sin ACK. Solo forense impreso + fotos. Plan A `dwnl`,
   plan B XMODEM en pausa.
4. **Un fix por iteración**, con señal de confirmación en log esperada.
5. **PID víctima, no ejecutor:** System limpiando a Halo mentía en logs
   globales. Attach-aware siempre (ver skill teardown).
6. **SEH no traga bugchecks** ni faults a dirección kernel sin dispatch.
   Ahí solo valen validación previa u orden correcto.

## 6. Protocolo de continuación

1. Pedir: shas desplegados + entrada freeldr + `GAMING-BUILD` + cola del log.
2. Descartar frankenkernel primero.
3. Diagnosticar con glosario de señales (§9 del contexto gigante).
4. Proponer siguiente fix/instrumentación con estado y binario portador.
5. Actualizar `GAMING-CHANGES.md`.

## 7. Pendientes abiertos (resumen)

Salida nondeterminista free-proc, `0x50` mode-switch en stack nuevo, Video1
`0x22`, 30→60 en pesadas (build `-O0`), ratón Win32 #31, RX serie, revertir
TEMP-DEBUG antes de publicar. Detalle en contexto gigante §8.
