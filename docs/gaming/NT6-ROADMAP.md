# NT6-Longterm — ReactOS NT6.0 real para gaming (rama de largo recorrido)

> Rama: `nt6-longterm` en `WallUG/reactos-gaming`
> (base: `main` @ `7846249175b [GAMING] Halo GT710 base`, que a su vez
>  desciende de `Longhorn-ReactOS-Stable` `328deb671c`).
> Remoto longhorn: `https://github.com/ReactOS-Longhorn-Initiative/reactos.git`
> (73 ramas + tags, fetcheadas a `longhorn/*`).
> Fecha de creación: 2026-09-14. Estado: **scaffolding + análisis — NO fusionar
>  a `main`**. `main` sigue siendo la base gaming estable (KSE + Halo).

---

## 1. Objetivo de esta rama

Construir, **fuera de `main`**, un ReactOS **NT 6.0 (Vista/WS08 6001)**
capaz de:

- Mantener lo gaming ya logrado (KSE + GT710 + Halo 3D + teardown/restore).
- Subir el target de versión a 6.0 para que instaladores/servicios/drivers
  que exigen Vista+ no fallen por `GetVersion`/`RtlGetVersion`.
- Abrir la puerta a **Steam / lanzadores** (vía `Longhorn-Steam-x64`) y a
  **KernelBase / APISET** modernos, sin romper KSE.

No es "cambiar una constante": NT6 toca `ntverp.h`, `sdkddkver.h`, `ntoskrnl`,
`ntdll`, `kernel32`, `kernelbase`, `apisets`, `ALPC`, `SMSS`, `boot` y todos
los tests. Por eso va en rama larga, con cherry-picks, no con merge bruto.

---

## 2. Qué aporta cada rama longhorn (inventario útil)

Inventario completo: `git branch -r | grep longhorn` (73 ramas).
Diffs medidos contra `longhorn/Longhorn-ReactOS-Stable`:

| Rama | Files | Resumen NT6 | ¿Usar en `nt6-longterm`? |
|---|---|---|---|
| **ReactOS-NT6-Compile** | 3455 (+130k/-1.04M) | **Tronco NT6**: `ntverp.h` → 6.0.6001, `sdkddkver` default NT6, `ntoskrnl` (ALPC, pushlock, resource, sysinfo, ke, mm, ps), `ntdll` (ldr/rtl + `nt_0600`), `kernel32`/`kernel32_vista` + `kernelbase` (vía Wine), `NT6 - ALPC` (`b787838`), `Default to NT6.0` (`9b63c30`), `Fix version to WS08` (`852769d`) | **SÍ — base candidata**. Pero elimina DWM/milcore (~1M líneas borradas: vuelve a XDDM clásico, por eso compila) |
| **Longhorn-KernelBaseBringup** | 2620 (+48k/-619k) | Bringup de `kernelbase.dll` real + apisets, `ntdll_vista.spec`, `ldrapi` NT6. Prerrequisito de Steam | SÍ — cherry-pick kernelbase/apisets |
| **Longhorn-Steam-x64** | 3243 (+91k/-642k) | NT6 + shims 64-bit + `ITaskbarList3/4`, `TLS`, `KernelBase` para Steam x64. Verde/roto | SÍ — como referencia de qué exige Steam (NT6.2+ en realidad) |
| **PR-ReactOS-NT6.0** | 13k files (rename-heavy) | PR histórico NT6 completo (predecesor de NT6-Compile) | Referencia, no directo |
| **Native-DirectX** | ~4300 (rename-heavy) | `modules/MSDX/` binarios MS reales (dxg.sys, d3d9/10/11). Camino "sin Wine" | SÍ — pero legal: binarios propietarios, jamás a upstream |
| **NT6KernelDrivers** | ~4700 | Drivers kernel adaptados a NT6 (WDF, storport, xHCI…) | SÍ a medio plazo (cuando NT6 base compile) |
| **NTSHIM_on_MS_ntkrnlmp** | ~5270 | Shim para correr sobre `ntkrnlmp` de MS — investigación, no prod | NO (solo lectura) |
| **longhorn-nvidia-uacpi-kshim** | 4494 (+233k/-759k) | **Nuestra base gaming**: KSE + uACPI + NVIDIA XDDM. Aquí nació `main` | Ya integrada en `main` |
| **Wddm-Win32k-Branch** | 560 (+11k/-141k) | DWM/milcore + WDDM (videoprt WDDM, dxg). Ruta larga a Win7+ drivers | Largo plazo, incompatible con NT6-Compile (éste lo borra) |
| **Tests-NT6_v3** | ~5270 | Tests NT6 (apitests/winetests). Útil para validar NT6 | SÍ — validar |
| **Longhorn-ReactOS-Stable** | — | Tronco estable gaming (nuestro `main` lo extiende) | Base actual |

**Conclusión:** `ReactOS-NT6-Compile` es la única rama que deja `NTDDI_VERSION >= NTDDI_VISTA` y `VER_PRODUCTMAJOR 6`. Pero lo hace **borrando DWM/milcore** y media pila gráfica — si la merges a lo bruto, pierdes `d3d11/dxgi` y rompes la vía WDDM. Hay que hacer **síntesis selectiva**, no merge.

---

## 3. Estrategia para NT6.0 real (sin romper gaming)

### Fase A — Congelar gaming en `main`, trabajar en `nt6-longterm`
- `main` no se toca salvo fixes gaming (#32+). `nt6-longterm` es la que se rompe.
- Cada cherry-pick debe compilar: `configure.sh` + `ninja -j12 bootcd` tras cada bloque.

### Fase B — Cherry-picks mínimos viables (orden)
1. **Version gate**: `sdk/include/psdk/ntverp.h` (ifdef `NTDDI_VISTA` → 6.0.6001) +
   `sdk/include/psdk/sdkddkver.h` default + `CMakeLists.txt` NTDDI.
   Log: `874731ed5 [SDK] Update ntverp.h`, `852769d [BOOT][SDK] FIx version`.
2. **NDK/ntoskrnl**: `ntoskrnl/ex/{init,pushlock,resource,sysinfo,win32k,work}`,
   `ke/{balmgr,dpc,procobj,thrdobj,thrdschd,time}`, `mm/ARM3/*`, `ps/{job,process,psmgr}`,
   `alpc/*`, `include/internal/*` + `sdk/include/ndk/*`.
   Logs: `b787838 [NT6.0]`, `2d0d6d7 [NT6 - ALPC]`, `fb64f05 scheduler vista`.
3. **ntdll/kernel32/kernelbase**: `dll/ntdll/{ldr,rtl,def,nt_0600}`,
   `dll/win32/kernel32/{client,kernel32_vista}`, `dll/win32/kernelbase/*`,
   `subsystems` + apisets. Logs: `4decc15 Fix NT5 flags`, `447dcd9 Remove TraceLogging`.
4. **Smoke test**: `bootcd.iso` arranca, `winver` dice 6.0, `GetVersionEx`/`RtlGetVersion` NT6.

### Fase C — Luego, y solo luego
- **KernelBaseBringup** (apisets) → probar `SteamSetup.exe` (fallará por NT6.2, pero sabremos qué pide).
- **Steam-x64** (x64 + TLS + TaskbarList) — solo si apuntas a Steam real (NT6.2+).
- **Native-DirectX/MSDX** vs **WDDM** — decisión excluyente a corto plazo (MSDX da 3D sin Wine ya; WDDM exige `dxgkrnl` que NT6-Compile no trae).
- **NT6KernelDrivers/WDF** — cuando la base NT6 sea estable.

### Qué NO hacer
- `git merge longhorn/ReactOS-NT6-Compile` directo: perderás DWM, romperás KSE (que miente NT 5.2).
- Portar todo `3455` ficheros: la mayoría son borrados de mspaint/netsh/msdx que nada aportan a NT6.
- Tocar `main` para NT6: rompería Halo hoy (KSE + teardown dependen de 5.2).

---

## 4. Remote y workflow

```bash
# ya configurado en este repo
git remote -v
# longhorn  https://github.com/ReactOS-Longhorn-Initiative/reactos.git
# origin    https://github.com/WallUG/reactos-gaming.git

git branch -a | grep longhorn  # 70+ ramas disponibles
git checkout nt6-longterm       # esta rama
# para traer una rama longhorn a local sin merge:
git fetch longhorn ReactOS-NT6-Compile --depth=50
git log --oneline longhorn/ReactOS-NT6-Compile --not longhorn/Longhorn-ReactOS-Stable | head

# cherry-pick selectivo (ejemplo version)
git checkout nt6-longterm
git cherry-pick 874731ed5  # ntverp
# verificar tras cada pick:
./configure.sh -DENABLE_ROSTESTS=1 -DENABLE_ROSAPPS=1
ninja -C build-nt6 bootcd -j12
```

Verificación: `strings` en `ntoskrnl.exe` debe decir `ProductVersion 6.0`,
`winver` en VM debe mostrar Vista. Si no, sigues en 5.2.

---

## 5. Estado actual y siguiente paso

- [x] Rama `nt6-longterm` creada desde `main` (`7846249175b`).
- [x] Remoto `longhorn` con 73 ramas fetcheadas.
- [x] Análisis diffs: NT6-Compile = 3455 ficheros, corazón NT6 en ~358 ficheros `sdk/ntoskrnl/ntdll/kernelbase`.
- [ ] Siguiente: cherry-pick #1 (ntverp/sdkddkver) + build smoke test.
- [ ] Luego: bloque ntoskrnl/ndk.

`GAMING-CHANGES.md` sigue siendo el registro gaming en `main`.
Esta rama lleva su propio `docs/gaming/NT6-ROADMAP.md` (este fichero).
Cuando NT6-Longterm sea jugable, se decide si se rebasea sobre `main` o se mantiene como `next`.

---

## 6. Advertencias (lecciones de ses_f85dce...)

- Todas las ramas longhorn rompen en GCC (solo compilan en MSVC) — portar `Vmr9.h→vmr9.h`, `C_ASSERT`, vtbls, etc., como ya hicimos.
- KSE miente 5.2 (`versionlie.c`); NT6 lo contradice. Habrá que parametrizar KSE por `MajorVersion`.
- El bug de perfil español (`IDS_LOCALLOWAPPDATA`) está en todas las ramas longhorn — ya fijado en `main`, re-aplicar en `nt6-longterm` si el cherry-pick no lo trae.
- `GAMING-BUILD` marker sigue en `nt6-longterm` (heredado de `main`).
