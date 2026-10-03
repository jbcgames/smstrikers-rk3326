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
# HARDWARE AUDIO SETUP
# =============================================================================
amixer -c 0 sset 'Playback Path' 'SPK' >/dev/null 2>&1 || true
amixer -c 0 sset 'Playback' 95% >/dev/null 2>&1 || true

# =============================================================================
# AUTO-DETECT MALI GPU DRIVER & CREATE SYMLINKS
# =============================================================================
MALI_DIR="/tmp/strikers_mali"
rm -rf "$MALI_DIR"
mkdir -p "$MALI_DIR"

if [ -f "$GAMEDIR/libs.aarch64/libmali-bifrost-g31-rxp0-gbm.so" ]; then
  MALI_BLOB="$GAMEDIR/libs.aarch64/libmali-bifrost-g31-rxp0-gbm.so"
elif [ -f "/usr/lib/aarch64-linux-gnu/libmali-bifrost-g31-rxp0-gbm.so" ]; then
  MALI_BLOB="/usr/lib/aarch64-linux-gnu/libmali-bifrost-g31-rxp0-gbm.so"
elif [ -f "/roms/ports/foxhollow/libs.aarch64/libmali-bifrost-g31-rxp0-gbm.so" ]; then
  MALI_BLOB="/roms/ports/foxhollow/libs.aarch64/libmali-bifrost-g31-rxp0-gbm.so"
else
  MALI_BLOB=$(find /usr/lib -name "libmali-bifrost-g31-*.so" -o -name "libmali.so" 2>/dev/null | head -n 1)
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
MAP_GO="190000004b4800000011000000010000,GO-Super Gamepad,a:b1,b:b0,back:b12,dpdown:b9,dpleft:b10,dpright:b11,dpup:b8,guide:b16,leftshoulder:b4,leftstick:b14,lefttrigger:b6,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b15,righttrigger:b7,rightx:a2,righty:a3,start:b13,x:b2,y:b3,platform:Linux,"
MAP_GO_DEB="1900bb3e4b4800000011000000010000,GO-Super Gamepad,a:b1,b:b0,back:b12,dpdown:b9,dpleft:b10,dpright:b11,dpup:b8,guide:b16,leftshoulder:b4,leftstick:b14,lefttrigger:b6,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b15,righttrigger:b7,rightx:a2,righty:a3,start:b13,x:b2,y:b3,platform:Linux,"

if [ -n "$sdl_controllerconfig" ]; then
  cfg_deb=$(echo "$sdl_controllerconfig" | sed 's/190000004b4800000011000000010000/1900bb3e4b4800000011000000010000/g')
  export SDL_GAMECONTROLLERCONFIG="${sdl_controllerconfig}"$'\n'"${cfg_deb}"$'\n'"${MAP_GO}"$'\n'"${MAP_GO_DEB}"
else
  export SDL_GAMECONTROLLERCONFIG="${MAP_GO}"$'\n'"${MAP_GO_DEB}"
fi

BIN="$GAMEDIR/strikers"
chmod +x "$BIN"

# =============================================================================
# CONTROLLER HOTKEY DAEMON (SELECT + START EXIT)
# =============================================================================
if [ -f "$GAMEDIR/strikers.gptk" ]; then
  if [ -n "$GPTOKEYB2" ]; then
    $GPTOKEYB2 "$(basename "$BIN")" -c "$GAMEDIR/strikers.gptk" &
  elif [ -n "$GPTOKEYB" ]; then
    $GPTOKEYB "$(basename "$BIN")" -c "$GAMEDIR/strikers.gptk" &
  fi
fi

pm_platform_helper "$BIN"

# =============================================================================
# ASSET DISCOVERY
# =============================================================================
if [ -d "$GAMEDIR/files" ]; then
  export STRIKERS_DATA="$GAMEDIR/files"
elif [ -f "$GAMEDIR/G4QE01.iso" ]; then
  export STRIKERS_DATA="$GAMEDIR/G4QE01.iso"
elif [ -f "$GAMEDIR/G4QE01.gcz" ]; then
  export STRIKERS_DATA="$GAMEDIR/G4QE01.gcz"
else
  for iso in "$GAMEDIR"/*.iso "$GAMEDIR"/*.ISO "$GAMEDIR"/*.gcz "$GAMEDIR"/*.gcm; do
    if [ -f "$iso" ]; then
      export STRIKERS_DATA="$iso"
      break
    fi
  done
fi

echo "=== Launching Super Mario Strikers (DATA=$STRIKERS_DATA) ==="
"$BIN"
ret=$?

echo "=== Super Mario Strikers exited with code $ret ==="
pm_finish
