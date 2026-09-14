---
name: reactos-input-mouse
description: Use when fixing FPS mouse aim stuck at screen edges, dinput warp, ClipCursor, SetCursorPos with Halo on ReactOS.
---

# ReactOS Input Mouse — del borde que atrapa al giro infinito

Cubre #29, #31. Halo en ReactOS sufre atrapamiento en bordes por dos vías
distintas; hay que distinguirlas por log antes de tocar código.

## 1. #29 dinput warp-check (PERMANENT) — `dll/directx/wine/dinput/mouse.c`

Bug: `warp_check` solo recentraba UNA vez por acquire (`if (!clipped)`);
después el cursor vagaba dentro del clip hasta clavarse en borde y los deltas
morían (ventana y completa).
Fix:
```c
const DWORD interval = 10; // antes: clipped ? 500 : 10
// ...
mapped_center.x = (rect.left + rect.right) / 2;
mapped_center.y = (rect.top + rect.bottom) / 2;
SetCursorPos(mapped_center.x, mapped_center.y);
```
Recentrar siempre en cada poll exclusivo (throttle 10 ms) + mantener clip.
El move sintético da delta cero por construcción (hook==cursor==centro).
Marker `GAMING-MOUSE exclusive acquire` (luego lifecycle completo).

## 2. Descubrimiento clave: Halo NO usa dinput para el ratón

Evidencia: `dinput loaded by pid` OK (sha `962a5cf0…`, 274944 bytes,
`certutil` coincide en Windows y ReactOS) pero cero `acquire` de mouse;
teclado sí adquiere. `findstr dinput8.dll halo.exe` confirma soporte, pero el
juego usa vía Win32: `ClipCursor` + posiciones absolutas sin recentrar.
Por eso el fix #29 no enganchaba aunque la DLL estuviera bien desplegada.

## 3. #31 traza Win32 (pendiente fix) — `win32ss/user/ntuser/cursoricon.c` + user32

Instrumentación en `win32k_CLEANUP.sys` (`0870a1e6…`, 3 markers en binario;
ojo: ese fichero usa `ERR`, no `DPRINT1`):
- `GAMING-MOUSE ClipCursor (l,t)-(r,b)` / `unclip` — raro, siempre.
- `GAMING-MOUSE SetCursorPos #N (x,y)` — 3 primeras + 1/240 (~1 línea/4 s a 60 fps).
Visto: clip `(43,24)-(1323,744)` repetido + `SetCursorPos hook 1` = movimientos
físicos del usuario, NO recentrado del juego → clavado en bordes por diseño
del fallback absoluto.

## 4. Ciclo de vida dinput (para no cazar fantasmas)

Markers añadidos por iteración (verificar los 5 en binario):
`DirectInputCreate` → `EnumDevices type …` → `acquire coop=… (EXCLUSIVE/...) 
warp_override=… res=…` → `WH_MOUSE_LL install FAILED?`.
Casos:
- Sin `dinput loaded` → swap no entró (nombre exacto `dinput.dll`, move offline,
  tamaño/fecha de hoy).
- `loaded` sin `acquire` mouse → juego 100% Win32 (fijar en win32k).
- `acquire res=0x80070005` → fallo foco/ventana/hook (atacar eso).
- `acquire res=0 EXCLUSIVE` → recentrado activo: probar giros 360°.

## 5. Protocolo

1. Desplegar `dinput_MOUSE.dll`→`dinput.dll` offline, verificar
   `certutil -hashfile C:\ReactOS\system32\dinput.dll SHA256`.
2. Entrar a Halo, girar 360° ambos sentidos, mover 30 s, pegar `GAMING-MOUSE`.
3. Si confirma vía Win32: decidir recentrado asistido en win32k vs
   `MouseWarpOverride=force` (solo si el juego tolera dinput).
4. No confundir `hook 1` (movimiento físico) con recentrado del juego.
