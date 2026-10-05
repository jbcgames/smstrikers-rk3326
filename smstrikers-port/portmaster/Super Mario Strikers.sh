#!/bin/bash
# PortMaster Launch Script for Super Mario Strikers
# Target: RK3326 / Mali-G31 MP2 / OpenGL ES 3.0 / KMSDRM

# Pre-kill any stale instances to ensure DRM master is completely free
sudo killall -9 strikers gptokeyb gptokeyb2 2>/dev/null || true

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi

source "$controlfolder/control.txt"
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
get_controls

GAMEDIR="/roms/ports/strikers"
cd "$GAMEDIR" || exit 1

> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1

# =============================================================================
# HARDWARE AUDIO SETUP (Preserve EmulationStation volume)
# =============================================================================
amixer -c 0 sset 'Playback Path' 'SPK' >/dev/null 2>&1 || true

# =============================================================================
# AUTO-DETECT MALI GPU DRIVER & CREATE SYMLINKS
# =============================================================================
MALI_DIR="/tmp/strikers_mali"
rm -rf "$MALI_DIR"
mkdir -p "$MALI_DIR"

if [ -f "/usr/local/lib/aarch64-linux-gnu/libmali-bifrost-g31-rxp0-gbm.so" ]; then
  MALI_BLOB="/usr/local/lib/aarch64-linux-gnu/libmali-bifrost-g31-rxp0-gbm.so"
elif [ -f "/usr/lib/aarch64-linux-gnu/libmali-bifrost-g31-rxp0-gbm.so" ]; then
  MALI_BLOB="/usr/lib/aarch64-linux-gnu/libmali-bifrost-g31-rxp0-gbm.so"
elif [ -f "/usr/lib/aarch64-linux-gnu/libmali-bifrost-g31-rxp0-wayland-gbm.so" ]; then
  MALI_BLOB="/usr/lib/aarch64-linux-gnu/libmali-bifrost-g31-rxp0-wayland-gbm.so"
elif [ -f "/usr/lib/aarch64-linux-gnu/libMali.so" ]; then
  MALI_BLOB="/usr/lib/aarch64-linux-gnu/libMali.so"
elif [ -f "/usr/lib/libMali.so" ]; then
  MALI_BLOB="/usr/lib/libMali.so"
elif [ -f "$GAMEDIR/libs.aarch64/libmali-bifrost-g31-rxp0-gbm.so" ]; then
  MALI_BLOB="$GAMEDIR/libs.aarch64/libmali-bifrost-g31-rxp0-gbm.so"
elif [ -f "/roms/ports/foxhollow/libs.aarch64/libmali-bifrost-g31-rxp0-gbm.so" ]; then
  MALI_BLOB="/roms/ports/foxhollow/libs.aarch64/libmali-bifrost-g31-rxp0-gbm.so"
else
  MALI_BLOB=$(find /usr/lib /usr/local/lib -name "libmali-bifrost-g31-*.so" -o -name "libmali.so" 2>/dev/null | head -n 1)
fi

if [ -n "$MALI_BLOB" ]; then
  echo "[port] Using Mali driver: $MALI_BLOB"
  ln -sf "$MALI_BLOB" "$MALI_DIR/libEGL.so.1"
  ln -sf "$MALI_BLOB" "$MALI_DIR/libEGL.so"
  ln -sf "$MALI_BLOB" "$MALI_DIR/libGLESv2.so.2"
  ln -sf "$MALI_BLOB" "$MALI_DIR/libGLESv2.so"
  ln -sf "$MALI_BLOB" "$MALI_DIR/libgbm.so.1"
  ln -sf "$MALI_BLOB" "$MALI_DIR/libgbm.so"
fi

export LD_LIBRARY_PATH="$MALI_DIR:$GAMEDIR/libs.aarch64:$GAMEDIR:$GAMEDIR/libs:$LD_LIBRARY_PATH"
unset LD_PRELOAD

export SDL_VIDEODRIVER="sdl2"
export SDL3SHIM_SDL2_VIDEODRIVER="kmsdrm"
export SDL_AUDIODRIVER="sdl2"
export SDL3SHIM_SDL2_AUDIODRIVER="alsa"

# =============================================================================
# STRIKERS PERFORMANCE PROFILE FOR RK3326 / LOW-SPEC GLES
# =============================================================================
if [ -f "$GAMEDIR/graphics_options.env" ]; then
  echo "[port] Sourcing custom graphics options from $GAMEDIR/graphics_options.env"
  source "$GAMEDIR/graphics_options.env"
fi

export STRIKERS_ENABLE_DOF="${STRIKERS_ENABLE_DOF:-0}"
export STRIKERS_CULL_EXTRA_MODELS="${STRIKERS_CULL_EXTRA_MODELS:-1}"
export STRIKERS_MAX_PARTICLES="${STRIKERS_MAX_PARTICLES:-512}"
export STRIKERS_FENCE_SPARKS="${STRIKERS_FENCE_SPARKS:-0}"
export STRIKERS_FENCE_RATE="${STRIKERS_FENCE_RATE:-0.06}"
# Cutscene/NIS optimizations (0 = disabled in cutscenes for max performance, 1 = enabled)
export STRIKERS_CUTSCENE_CROWD="${STRIKERS_CUTSCENE_CROWD:-0}"
export STRIKERS_CUTSCENE_SHADOWS="${STRIKERS_CUTSCENE_SHADOWS:-0}"
export STRIKERS_CUTSCENE_EFFECTS="${STRIKERS_CUTSCENE_EFFECTS:-0}"
export STRIKERS_CUTSCENE_CULL_EXTRA="${STRIKERS_CUTSCENE_CULL_EXTRA:-1}"
export STRIKERS_CUTSCENE_MAX_DIST="${STRIKERS_CUTSCENE_MAX_DIST:-85.0}"
export STRIKERS_CUTSCENE_LOD="${STRIKERS_CUTSCENE_LOD:-0.035}"
export STRIKERS_CUTSCENE_SKIN="${STRIKERS_CUTSCENE_SKIN:-rigid}"
# Scene transitions / Wipes (1 = fast smooth fade to white, cut = instant cut, 0 = heavy 3D transitions)
export STRIKERS_FAST_TRANSITIONS="${STRIKERS_FAST_TRANSITIONS:-1}"
export STRIKERS_TRANSITION_SPEED="${STRIKERS_TRANSITION_SPEED:-1.0}"
export STRIKERS_DISABLE_SCREENGRAB="${STRIKERS_DISABLE_SCREENGRAB:-1}"
export STRIKERS_DISABLE_TRANSITION_OUTLINE="${STRIKERS_DISABLE_TRANSITION_OUTLINE:-1}"

# =============================================================================
# HARDWARE PERFORMANCE GOVERNORS & MEMORY TUNING
# =============================================================================
for cpu in /sys/devices/system/cpu/cpu[0-3]/online; do
  [ -f "$cpu" ] && echo 1 | sudo tee "$cpu" >/dev/null 2>&1 || true
done

if [ -f /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor ]; then
  echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor >/dev/null 2>&1 || true
fi

for gpu_gov in /sys/devices/platform/*.gpu/devfreq/*.gpu/governor /sys/class/devfreq/*gpu*/governor; do
  [ -f "$gpu_gov" ] && echo performance | sudo tee "$gpu_gov" >/dev/null 2>&1 || true
done

for dmc_gov in /sys/devices/platform/dmc/devfreq/dmc/governor /sys/class/devfreq/*dmc*/governor; do
  [ -f "$dmc_gov" ] && echo performance | sudo tee "$dmc_gov" >/dev/null 2>&1 || true
done

echo ark | sudo -S /sbin/sysctl -w vm.overcommit_memory=1 >/dev/null 2>&1 || true
echo ark | sudo -S /sbin/sysctl -w vm.min_free_kbytes=32768 >/dev/null 2>&1 || true
echo ark | sudo -S /sbin/sysctl -w vm.vfs_cache_pressure=200 >/dev/null 2>&1 || true
echo ark | sudo -S /sbin/sysctl -w vm.swappiness=100 >/dev/null 2>&1 || true
echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null 2>&1 || true

# Auto-protect strikers process from OOM killer
(
  for _ in {1..30}; do
    PID=$(pgrep -x strikers | head -n 1)
    if [ -n "$PID" ]; then
      echo ark | sudo -S sh -c "echo -800 > /proc/$PID/oom_score_adj" 2>/dev/null
      break
    fi
    sleep 0.5
  done
) &

# =============================================================================
# GAMEPAD CONFIGURATION
# =============================================================================
# Default RK3326 mapping (a0=LX, a1=LY, a2=RX, a3=RY)
MAP_PLAY_JOYSTICK="19000f6a706c61795f6a6f7973746900,play_joystick,a:b0,b:b1,x:b2,y:b3,back:b8,guide:b10,start:b9,leftstick:b11,rightstick:b12,leftshoulder:b4,rightshoulder:b5,dpup:b14,dpdown:b15,dpleft:b16,dpright:b17,leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:b6,righttrigger:b7,platform:Linux,"
MAP_PLAY_JOYSTICK_ALT="19000000000000000000000000000000,play_joystick,a:b0,b:b1,x:b2,y:b3,back:b8,guide:b10,start:b9,leftstick:b11,rightstick:b12,leftshoulder:b4,rightshoulder:b5,dpup:b14,dpdown:b15,dpleft:b16,dpright:b17,leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:b6,righttrigger:b7,platform:Linux,"

MAP_GO="190000004b4800000011000000010000,GO-Super Gamepad,a:b1,b:b0,back:b12,dpdown:b9,dpleft:b10,dpright:b11,dpup:b8,guide:b16,leftshoulder:b4,leftstick:b14,lefttrigger:b6,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b15,righttrigger:b7,rightx:a2,righty:a3,start:b13,x:b2,y:b3,platform:Linux,"
MAP_GO_DEB="1900bb3e4b4800000011000000010000,GO-Super Gamepad,a:b1,b:b0,back:b12,dpdown:b9,dpleft:b10,dpright:b11,dpup:b8,guide:b16,leftshoulder:b4,leftstick:b14,lefttrigger:b6,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b15,righttrigger:b7,rightx:a2,righty:a3,start:b13,x:b2,y:b3,platform:Linux,"
MAP_OGA="190000004b4800000010000001010000,GO-Advance Gamepad (rev 1.1),a:b1,b:b0,back:b12,dpdown:b9,dpleft:b10,dpright:b11,dpup:b8,leftshoulder:b4,leftstick:b13,lefttrigger:b14,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b16,righttrigger:b15,start:b17,x:b2,y:b3,platform:Linux,"
MAP_GAMEFORCE="19000000030000000300000002030000,gameforce_gamepad,a:b1,b:b0,back:b8,dpdown:b11,dpleft:b12,dpright:b13,dpup:b10,guide:b16,leftshoulder:b4,leftstick:b14,lefttrigger:b6,leftx:a1,lefty:a0,rightshoulder:b5,rightstick:b15,righttrigger:b7,rightx:a3,righty:a2,start:b9,x:b2,y:b3,platform:Linux,"

DEFAULT_MAPS="${MAP_PLAY_JOYSTICK}"$'\n'"${MAP_PLAY_JOYSTICK_ALT}"$'\n'"${MAP_GO}"$'\n'"${MAP_GO_DEB}"$'\n'"${MAP_OGA}"$'\n'"${MAP_GAMEFORCE}"

# Priority 1: User custom controls environment configured via "Super Mario Strikers Controls.sh"
if [ -f "$GAMEDIR/custom_controls.env" ]; then
  echo "[port] Sourcing custom controls environment from $GAMEDIR/custom_controls.env"
  source "$GAMEDIR/custom_controls.env"
fi

CUSTOM_MAP=""
if [ -f "$GAMEDIR/custom_controls.txt" ]; then
  CUSTOM_MAP=$(grep -v '^#' "$GAMEDIR/custom_controls.txt" | head -n 1)
  if [ -n "$CUSTOM_MAP" ]; then
    echo "[port] Using custom controls configuration from $GAMEDIR/custom_controls.txt"
  fi
fi

if [ -n "$CUSTOM_MAP" ]; then
  # Put CUSTOM_MAP LAST so SDL3 prioritizes it over DEFAULT_MAPS!
  export SDL_GAMECONTROLLERCONFIG="${DEFAULT_MAPS}"$'\n'"${CUSTOM_MAP}"
else
  export SDL_GAMECONTROLLERCONFIG="${DEFAULT_MAPS}"
fi

if [ -n "$sdl_controllerconfig" ]; then
  cfg_deb=$(echo "$sdl_controllerconfig" | sed 's/190000004b4800000011000000010000/1900bb3e4b4800000011000000010000/g')
  export SDL_GAMECONTROLLERCONFIG="${SDL_GAMECONTROLLERCONFIG}"$'\n'"${sdl_controllerconfig}"$'\n'"${cfg_deb}"
fi

BIN="$GAMEDIR/strikers"
chmod +x "$BIN"

# Native exit combo (Select + Start) is handled directly by the engine in C++.

pm_platform_helper "$BIN"

# =============================================================================
# ASSET DISCOVERY (Prioriza ROM Europea G4QP01, USA G4QE01, Japón G4QJ01 o dump)
# =============================================================================
if [ -f "$GAMEDIR/G4QP01.iso" ]; then
  export STRIKERS_DATA="$GAMEDIR/G4QP01.iso"
elif [ -f "$GAMEDIR/G4QP01.gcz" ]; then
  export STRIKERS_DATA="$GAMEDIR/G4QP01.gcz"
elif [ -f "$GAMEDIR/G4QE01.iso" ]; then
  export STRIKERS_DATA="$GAMEDIR/G4QE01.iso"
elif [ -f "$GAMEDIR/G4QE01.gcz" ]; then
  export STRIKERS_DATA="$GAMEDIR/G4QE01.gcz"
elif [ -f "$GAMEDIR/G4QJ01.iso" ]; then
  export STRIKERS_DATA="$GAMEDIR/G4QJ01.iso"
elif [ -f "$GAMEDIR/G4QJ01.gcz" ]; then
  export STRIKERS_DATA="$GAMEDIR/G4QJ01.gcz"
elif [ -d "$GAMEDIR/files" ]; then
  export STRIKERS_DATA="$GAMEDIR/files"
else
  for iso in "$GAMEDIR"/*.iso "$GAMEDIR"/*.ISO "$GAMEDIR"/*.gcz "$GAMEDIR"/*.GCZ "$GAMEDIR"/*.gcm "$GAMEDIR"/*.ciso; do
    if [ -f "$iso" ]; then
      export STRIKERS_DATA="$iso"
      break
    fi
  done
fi

# =============================================================================
# EXECUTION & RELOAD LOOP (Permite reiniciar al cambiar de idioma en el juego)
# =============================================================================
while true; do
  if [ -f "$GAMEDIR/language.env" ]; then
    echo "[port] Sourcing language setting from $GAMEDIR/language.env"
    source "$GAMEDIR/language.env"
  fi
  export STRIKERS_LANGUAGE="${STRIKERS_LANGUAGE:-spanish}"
  echo "[port] Selected game language: $STRIKERS_LANGUAGE"

  echo "=== Launching Super Mario Strikers (DATA=$STRIKERS_DATA) ==="
  "$BIN"
  ret=$?
  echo "=== Super Mario Strikers exited with code $ret ==="

  if [ $ret -eq 42 ]; then
    echo "[port] Game requested restart to apply language changes, reloading..."
    sleep 0.5
    continue
  fi
  break
done

pm_finish
