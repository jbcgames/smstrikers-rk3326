# Super Mario Strikers (RK3326 / OpenGL ES 3.0 Port)

Este repositorio contiene la adaptación y cross-compilación nativa de **Super Mario Strikers** (Nintendo GameCube, ID `G4QE01`) para dispositivos basados en el SoC **Rockchip RK3326** (GPU ARM Mali-G31 MP2, 1GB RAM) bajo sistemas **ArkOS / PortMaster** (como Anbernic RG351P/M/V/MP, R35S, R36S, Powkiddy RGB20S, Gameforce Chi, etc.).

---

## 🎮 Origen y Reconocimientos

- **Proyecto Base:** Basado en el port para PC desarrollado por [new-coke/strikers](https://github.com/new-coke/strikers).
- **Capa Gráfica OpenGL ES:** Basada en la reimplementación y fork de Aurora GLES desarrollada por **bmdhacks** (utilizada exitosamente en ports como *Foxhollow / Star Fox Adventures*, *Mario Party 4*, *Super Smash Bros. Melee*).
- **Adaptación y optimización RK3326:** Jbcgames / Comunidad PortMaster.

---

## ⚙️ Modificaciones Principales Realizadas

1. **Eliminación Total de Dawn y WebGPU:**
   - Dawn / Tint / WebGPU no son viables en RK3326 debido a la falta de soporte Vulkan completo y la limitación de bloques SSBO (`GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS = 0`) en los controladores GLES propietarios de ARM Mali Bifrost.
   - Se migró el subsistema gráfico a **Aurora GLES 3.0** (`bmdhacks`), realizando compilación dinámica de shaders GLSL ES 3.0 en tiempo de ejecución con almacenamiento y reutilización en caché binaria (`glProgramBinary`).

2. **Soporte de Skinning Esquelético de Personajes (`MaxPnMtx = 13`):**
   - A diferencia de otros juegos de GameCube que utilizan 10 registros de matrices de posición/normal, *Super Mario Strikers* requiere 13 matrices (`MaxPnMtx = 13`) en las filas 30 a 36 de la memoria XF para el cálculo de huesos y animaciones de los capitanes y compañeros de equipo.
   - Se actualizaron `gx.hpp`, `regs.cpp`, `GXTransform.cpp` y `shader.cpp` para alojar y mapear correctamente estas matrices sin desbordar memoria ni truncar transformaciones.

3. **Manejo Completo de `GX_CULL_ALL`:**
   - El motor de *Super Mario Strikers* invoca el modo de descarte `GX_CULL_ALL` (valor 3) en fases específicas del renderizado del terreno y cinemáticas.
   - Se implementó la traducción en `to_cull_mode` y `build_pipeline` en `gx.cpp`, desactivando las máscaras de escritura de color y profundidad para descartar la geometría limpiamente en lugar de provocar un aborto fatal (`DEFAULT_FATAL`).

4. **Presentación KMSDRM vía SDL3-over-SDL2 Shim:**
   - La capa de presentación utiliza el shim `libSDL3.so.0` sobre `SDL2`, permitiendo escaneo directo a pantalla mediante el backend **KMSDRM** contra el blob Mali Bifrost (`libmali-bifrost-g31-rxp0-gbm.so`) y audio de baja latencia con driver **ALSA**.

5. **Desacoplamiento de Dependencias de Escritorio:**
   - Se desconectó la integración con Discord RPC y la interfaz Borealis (`STRIKERS_DISCORD=OFF`), sustituyéndola por stubs no-op ligeros en `src/platform/discord_stub.c`.
   - Se implementaron stubs para telemetría y perfiles de texturas (`aurora_gfx_pool_stats`, `aurora_gfx_texture_stats`, `aurora_replacement_*`).

---

## 🛠️ Requisitos de Compilación

Para compilar el proyecto se requiere un entorno Linux x86_64 (Linux nativo o WSL2 Ubuntu 20.04/22.04/24.04):

- **Compiladores y herramientas:**
  - `clang` (versión 14 o superior)
  - `llvm-nm` (indispensable para la resolución de grafos de símbolos)
  - `aarch64-linux-gnu-gcc` / `aarch64-linux-gnu-g++` (cross-toolchain GNU)
  - `cmake` (>= 3.28)
  - `ninja-build`
  - `python3`
- **Bibliotecas y Sysroot AArch64:**
  - Sysroot con cabeceras de `SDL2`, `libdrm`, `zlib`, `libpng`.
  - Shim `libSDL3.so` (compilado para AArch64 sobre SDL2).
  - Herramienta `aarch64-linux-gnu-strip` para optimizar el tamaño del binario.

---

## 🚀 Instrucciones de Compilación (Cross-Compilación para AArch64)

### 1. Configuración de CMake
Utilizando el preset preconfigurado `linux-aarch64-gles`:
```bash
cmake --preset linux-aarch64-gles
```

*(O de forma manual si se desea personalizar rutas)*:
```bash
cmake -B build/linux-aarch64-gles -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64-linux-gnu.cmake \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DAURORA_GLES=ON \
  -DAURORA_SDL3_PROVIDER=vendor \
  -DSTRIKERS_DISCORD=OFF \
  -DSTRIKERS_FFMPEG=OFF
```

### 2. Compilar Bibliotecas y Verificación de Unidades de Traducción
```bash
# 1. Compilar todas las unidades de código del juego
cmake --build build/linux-aarch64-gles --target strikers_scan -j$(nproc)

# 2. Generar las bibliotecas estáticas de Aurora
cmake --build build/linux-aarch64-gles --target aurora_card aurora_core aurora_gx aurora_pad aurora_vi aurora_main aurora_os aurora_mtx imgui -j$(nproc)
```

### 3. Resolución de Símbolos y Generación de Stubs
El proyecto cuenta con un generador automático de stubs para resolver las llamadas al hardware original de GameCube que la capa de compatibilidad no requiere:
```bash
STRIKERS_BUILD_DIR=build/linux-aarch64-gles python3 tools/genstubs.py --noop '^GX|^snd|^AI|^AR' --accept-new-stubs
```

### 4. Compilación del Ejecutable Final y Reducción de Tamaño
```bash
# Compilar ejecutable
cmake --build build/linux-aarch64-gles --target strikers -j$(nproc)

# Reducir tamaño eliminando símbolos de depuración
aarch64-linux-gnu-strip -s -o strikers.elf build/linux-aarch64-gles/strikers
```
El binario resultante optimizado tiene un peso aproximado de **6.8 MB**.

---

## 📦 Extracción de Assets (Sin Distribución de ROMs)

> ⚠️ **AVISO LEGAL:** Este repositorio **NO contiene código con copyright de Nintendo ni archivos de datos/ROMs del juego**. Para ejecutar el port, debes ser propietario de una copia legal de *Super Mario Strikers* para Nintendo GameCube.

Para preparar los archivos de datos a partir de tu imagen de disco ISO:

```bash
python3 tools/extract-disc.py "ruta/a/G4QE01.iso" data/G4QE01
```

Esto generará la carpeta `data/G4QE01/files` con los **1416 archivos del juego** (aproximadamente 645 MB).

---

## 🕹️ Instalación y Estructura en PortMaster (ArkOS)

Copia los archivos a la tarjeta SD de tu consola en la partición `/roms/ports/`:

```
/roms/ports/
├── Super Mario Strikers.sh          <-- Lanzador principal para EmulationStation
└── strikers/
    ├── strikers                     <-- Binario ejecutable 'strikers.elf'
    ├── Super Mario Strikers.sh      <-- Copia del script lanzador
    ├── strikers.gptk                <-- Mapeo de controles y hotkeys
    ├── port.json                    <-- Metadatos para PortMaster
    ├── files/                       <-- Carpeta con los 1416 archivos extraídos del juego
    └── libs.aarch64/
        ├── libSDL3.so.0             <-- Shim SDL3-over-SDL2
        ├── libSDL3.so
        ├── libpng16.so.16
        ├── libz.so.1
        └── libmali-bifrost-g31-rxp0-gbm.so
```

### Controles en Consola Portátil:
- **Movimiento:** Stick análogo izquierdo / D-Pad.
- **Pase / Tiro / Acción:** Botones `A`, `B`, `X`, `Y`.
- **Gatillos / Habilidades:** `L1`, `L2`, `R1`, `R2`.
- **Salir del Juego:** Presionar simultáneamente **Select + Start**.
