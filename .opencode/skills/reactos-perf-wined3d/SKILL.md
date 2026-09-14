---
name: reactos-perf-wined3d
description: Use when investigating ReactOS gaming fps, wined3d draws, present timing, CSMT, VRAM fallback, vsync caps in Halo with NVIDIA GT 710.
---

# ReactOS Perf wined3d — de 3-5 fps a 30 y 60 fps

Cubre #25–#28. Pipeline confirmado HW; el impuesto estaba en CPU por draw.

## 1. Punto de partida y descartes

- Síntoma: Halo 3D a 3-5 fps con GT 710 (debería sobrar).
- Descartado serie: en estable ~1 línea/2 s, insignificante.
- Descartado GPU: `GL_VENDOR NVIDIA`, `GL_VERSION 4.4.0 NVIDIA 353.62`,
  `card GTX 470 (10de:06cd) vidmem 1280 MB`, `software 0x0` (HW vertex),
  `GL_RENDERER unknown board/PCIe/SSE2`.
- Descartado VRAM=0: fallback a 1280 MB de la DB no mueve fps (#27).
- Pistas: a 640x480 todo bajo los fps NO cambian (no fill-bound);
  mirando al cielo (sin objetos) SUBEN (coste por draw/objeto).

## 2. Instrumentación (TEMP-DEBUG, swap offline)

En `dll/directx/wine/wined3d/`:
- `adapter_gl.c`: `FIXME GAMING-PERF GL_RENDERER/VENDOR/VERSION` (una vez) +
  `GAMING-PERF card: %s (%04x:%04x), vidmem %u MB, emulated vram 0x%s` +
  fallback `vram was 0, using card DB` si ICD sin `WGL_WINE_query_renderer`.
- `d3d9/directx.c`: `GAMING-PERF CreateDevice … SOFTWARE/HARDWARE/MIXED`.
- `cs.c` + `swapchain.c`: contador `g_gaming_draw_count`
  (`InterlockedIncrement` en `emit_draw`) + fps-meter cada 120 presents:
  `GAMING-PERF fps: 120 frames in N ms (~F fps), present M ms total
  (gdi X / gl Y), draws D (~K/frame)`.
- Módulo Wine se llama `d3dwine.dll` (no `wined3d.dll`). Tamaños/shas varían
  por iteración; verificar por `strings` + tamaño + sha tras move offline.
- Buscar `GAMING-PERF`, no `GL_RENDERER` a secas (prefijo fixme Wine).

## 3. Resultados medidos

- Present inocente: <1 ms/frame, todo vía GL (`gdi 0 / gl 120`).
- Draws ~400/frame a ~0.5 ms con CSMT on. Menú y juego mismo impuesto fijo.
- 120 frames en ~22 s con present 0-105 ms total → ~190 ms/frame en draws.

## 4. #26 csmt=0 (DEFAULT GAMING, no binario)

wined3d dibuja por hilo separado (command stream); en SMP ayuda, en
monoprocesador ReactOS cada draw paga handoff entre hilos.
Fix sin recompilar:
`regedit → HKCU\Software\Wine\Direct3D` (crear) → DWORD `csmt=0`.
Log confirma `Setting multithreaded command stream to 0x0`.
Efecto: **5 fps → ~30 fps** estables en i3 monoprocesador.

## 5. #28 muro de los 30 y 60 fps

- Con `csmt=0`, 120 frames tardan 4018-4154 ms hagan 335 o 477 draws.
  Más draws ≠ más ms hasta ~900 draws → espera fija: vsync 60 Hz cayendo
  cada 2 vblanks o cap 30 fps del Halo. Frame clavado ~33.5 ms.
- Comprobar ajustes vídeo Halo: `Framerate 30 fps / VSync`, refresco 60 Hz.
  Si ya VSync y sigue 30: driver impone vsync y wined3d falla
  (`Failed to set swap interval 0`, `Something's still holding back buffer`).
  Entonces forzar intervalo 0 en swapchain por código.
- Hito: `csmt=0` + ajustes bajos, 120 frames en ~2010 ms = 59.7 fps
  (vsync 60 Hz; present absorbe 400-900 ms/120).
- Ley medida: `fps ~= 1/max(draws*80us, 16.7ms)`: ~200→60, ~400→30, ~1000→20.
- Pendiente 30→60 en pesadas: bajar coste/draw (build Debug `-O0` cuesta 2-5×;
  Release son horas — plan B).

## 6. Protocolo perf

1. Desplegar `d3dwine_PERF.dll`→`d3dwine.dll` offline, jugar 1 min en zona con
   objetos, pegar 2-3 líneas `GAMING-PERF fps`.
2. Si present >>1 ms/frame → mirar present/blits. Si draws altos → mirar draws.
3. Probar `csmt=0` antes de recompilar nada.
4. Probar resolución baja (¿escala? → fill; ¿igual? → CPU) y mirar al cielo
   (¿sube? → por-objeto).
