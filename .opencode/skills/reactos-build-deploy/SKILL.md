---
name: reactos-build-deploy
description: Use when building ReactOS with RosBE-Unix, fixing GCC 8.4 compile errors, porting KSE drivers, deploying fixes via HTTP dwnl or XMODEM serial.
---

# ReactOS Build y Deploy — compilar y llevar fixes al target sin sufrir

## 1. RosBE-Unix 2.2.1 en Kali

PPA (`ppa:reactos/rosbe-unix`, `apt install rosbe-unix`) solo Ubuntu 18.04/20.04.
En Kali: instalación manual (scripts, `RosBE-Version`, `amd64/version.sh`,
symlink `bin/ninja`). Rutas históricas `/home/demox/RosBE` → `/home/demox/RosBE-CI`.
Verificar: `i686/x86_64-w64-mingw32-gcc 8.4.0` + `configure.sh` OK.

## 2. Configurar y compilar

- WDDM (Windows): `output-MinGW-i386`.
- LH (Linux): `configure.sh -DENABLE_ROSTESTS=1 -DENABLE_ROSAPPS=1`,
  `ninja -j12 bootcd` → `bootcd.iso` ~504 MB en `/home/demox/build-LH-Stable`.
- Reloj WSL invierte mtimes → ninja salta recompilas (binario rancio).
  Ante duda `touch` fuentes + verificar por `strings`, no por fecha.

## 3. Fixes GCC 8.4 aplicados (patrón: pegar FAILED, editar mínimo)

- `av.h`: `Vmr9.h`→`vmr9.h` (mayúsculas, Linux sensible).
- `dwrite/freetype.c`: `C_ASSERT` + `ARRAYSIZE`→`sizeof`.
- `d2d1` winetests: wrappers WIDL, vtbls, imports; `CMakeLists.txt`.
- `dwrite` winetests: typo `-D-DWINETEST...`, vtbls, sequence-point;
  `font.c/layout.c`.
- `ntdll` winetests: psapi/spec exports (`advapi32.spec`, `kernel32.spec`).
- `ksecdd`: handles PVOID, `BCryptDestroyHash@4`.
- `rddm/displib/Dxgk.c`: NTAPI/coma.
- `milcore/av/av.h`, MediaFoundation/ffmpeg (`mfsrcsnk/media_source.c`),
  `winedmo` (`main.c/reactos.c/unix_private.h/unixlib.h`), `colorcnv`,
  `wmvdecod`.
- `d3d9/directx.c`, `dinput_main.c`, `dinput/mouse.c` (perf/mouse).
Todos `[PERMANENT]` salvo instrumentación.

## 4. KSE portado (driver 353.62 carga por esto)

Desde `longhorn-nvidia-uacpi-kshim`:
`ntoskrnl/kse/{kse.c (406),driverscope.c (346),versionlie.c (129)}`,
`ntoskrnl/include/internal/kse.h` (154), `media/sdb/drvmain.xml` (+54) +
`CMakeLists.txt` (+18), hooks en `ntos.cmake`, `ntoskrnl.h`, `io/iomgr/driver.c`,
`io/iomgr/iomgr.c`, `mm/ARM3/sysldr.c`, `sdk/include/ndk/iotypes.h`.
Efecto: `nv4_mini.sys` + `nv4_disp.dll` cargan (174 imports OK), OpenGL rinde.

## 5. Perfil español (bug instalación)

`dll/win32/userenv/lang/es-ES.rc` (+`IDS_LOCALLOWAPPDATA/IDS_ROAMINGAPPDATA`) +
`dll/win32/userenv/setup.c` (+22, crear directorios intermedios en
`CreateStandardProfile`). `[PERMANENT]`.

## 6. Despliegue plan A (operativo)

1. En Windows dev: `py -3 -m http.server 8080 -d C:\Users\garci`
   (o portproxy WSL: `netsh interface portproxy add v4tov4
   listenaddress=192.168.18.156 listenport=8080
   connectaddress=172.30.120.155 connectport=8080`).
2. En ReactOS: `mkdir C:\fx` + `dwnl http://192.168.18.156:8080/<fichero> C:\fx\...`.
3. Swap offline (nunca en caliente): kernels a `C:\` + entrada freeldr clonada;
   DLLs a `system32`; `dxg.sys` a `drivers`. Backup siempre.
4. Reboot a debug, buscar `GAMING-BUILD` con fecha de hoy.
5. Requiere red en ReactOS (`ipconfig` → `192.168.18.x`); si no, plan B.

## 7. Plan B XMODEM (en pausa por RX muerto)

`rx.c`/`xmsend.py` (XMODEM-CRC, reintento por bloque). `rx get <dest> [COMn]`
(defecto COM1) + `rx stage` (canje en arranque vía `PendingFileRenameOperations`).
A 115200 (~10 KB/s): DLL ~1 min, kernel ~8 min. Relleno `0x1A` final lo ignora
el PE. Transferir en entrada NO-debug (COM1 limpio). Desplegar `rx.exe` una vez
por plan A. Diagnóstico RX: `C` llegan pero sin ACK = TX adaptador→RX target roto.

## 8. Verificación (no negociable)

`sha256sum`/`certutil -hashfile … SHA256` + tamaño (ej. dinput 274944,
`nv4_mini.sys` 11152080, `nv4_disp.dll` 3513984) + `strings` con markers
(`GAMING-BUILD`, `safety net`, `dropped device-physical`, `APCTRACE`,
`GAMING-MOUSE`, `GAMING-PERF`). Fecha no vale.
