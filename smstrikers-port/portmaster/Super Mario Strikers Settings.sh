#!/bin/bash
# Super Mario Strikers - Configurador de Gráficos y Rendimiento
# Compatible con ArkOS / dArkOS / PortMaster / EmulationStation

# 1. Detectar terminal interactivo (SSH) o pantalla de consola (EmulationStation)
if [ -t 1 ]; then
  CURR_TTY="$(tty 2>/dev/null || echo /dev/tty1)"
else
  CURR_TTY="/dev/tty1"
fi

# 2. Configurar terminal, permisos y entorno para pantalla de consola
export TERM=linux
export XDG_RUNTIME_DIR="/run/user/$(id -u 2>/dev/null || echo 1002)"
sudo chmod 666 /dev/tty1 2>/dev/null || true
sudo chmod 666 /dev/uinput 2>/dev/null || true

# Fuente legible para pantallas pequeñas (3.5" 640x480)
if [ -f "/usr/share/consolefonts/Lat7-TerminusBold22x11.psf.gz" ]; then
  sudo setfont /usr/share/consolefonts/Lat7-TerminusBold22x11.psf.gz 2>/dev/null || true
elif [ -f "/usr/share/consolefonts/Lat7-Terminus16.psf.gz" ]; then
  sudo setfont /usr/share/consolefonts/Lat7-Terminus16.psf.gz 2>/dev/null || true
fi

# Configuración visual de dialog (evita sombras defectuosas en framebuffer)
if [ -f "/opt/inttools/noshadows.dialogrc" ]; then
  export DIALOGRC="/opt/inttools/noshadows.dialogrc"
fi

# Limpiar pantalla y ocultar cursor
printf "\033c" > "$CURR_TTY" 2>/dev/null || true
printf "\e[?25l" > "$CURR_TTY" 2>/dev/null || true

GAMEDIR="/roms/ports/strikers"
ENV_FILE="$GAMEDIR/graphics_options.env"
GPTK_FILE="$GAMEDIR/controls.gptk"

# 3. Iniciar daemon de controles para navegar con gamepad en dialog
pkill -9 -f "gptokeyb.*dialog" 2>/dev/null || true
GP_PID=""

if [ -f "/opt/inttools/gptokeyb" ] && [ -f "/opt/inttools/keys.gptk" ]; then
  export SDL_GAMECONTROLLERCONFIG_FILE="/opt/inttools/gamecontrollerdb.txt"
  /opt/inttools/gptokeyb -1 "dialog" -c "/opt/inttools/keys.gptk" >/dev/null 2>&1 &
  GP_PID=$!
elif [ -f "/opt/system/Tools/PortMaster/gptokeyb" ] && [ -f "$GPTK_FILE" ]; then
  export SDL_GAMECONTROLLERCONFIG_FILE="/opt/system/Tools/PortMaster/gamecontrollerdb.txt"
  /opt/system/Tools/PortMaster/gptokeyb -1 "dialog" -c "$GPTK_FILE" >/dev/null 2>&1 &
  GP_PID=$!
fi

cleanup() {
  if [ -n "$GP_PID" ]; then
    kill -9 "$GP_PID" 2>/dev/null || true
  fi
  pkill -9 -f "gptokeyb.*dialog" 2>/dev/null || true
  printf "\e[?25h" > "$CURR_TTY" 2>/dev/null || true
  printf "\033c" > "$CURR_TTY" 2>/dev/null || true
}
trap cleanup EXIT SIGINT SIGTERM

# Cargar configuración actual o establecer valores por defecto (Medio)
load_config() {
  OPT_PRESET="Medio"
  OPT_ASPECT="auto"
  OPT_RES_SCALE="0.85"
  OPT_SKIN="rigid"
  OPT_CROWD="0"
  OPT_SHADOWS="0"
  OPT_GAMEPLAY_SHADOWS="1"
  OPT_EFFECTS="0"
  OPT_CULL_EXTRA="1"
  OPT_MAX_DIST="85.0"
  OPT_LOD="0.035"
  OPT_FAST_TRANS="1"
  OPT_NO_SCREENGRAB="1"
  OPT_NO_OUTLINE="1"
  OPT_CULL_MODELS="1"
  OPT_MAX_PARTICLES="512"
  OPT_FENCE_SPARKS="0"
  OPT_FENCE_RATE="0.06"
  OPT_ENABLE_DOF="0"
  OPT_ENABLE_REPLAYS="1"

  if [ -f "$ENV_FILE" ]; then
    source "$ENV_FILE"
    OPT_PRESET="${STRIKERS_PRESET:-Personalizado}"
    OPT_ASPECT="${STRIKERS_ASPECT:-auto}"
    OPT_RES_SCALE="${STRIKERS_RES_SCALE:-0.85}"
    OPT_SKIN="${STRIKERS_CUTSCENE_SKIN:-rigid}"
    OPT_CROWD="${STRIKERS_CUTSCENE_CROWD:-0}"
    OPT_SHADOWS="${STRIKERS_CUTSCENE_SHADOWS:-0}"
    OPT_GAMEPLAY_SHADOWS="${STRIKERS_GAMEPLAY_SHADOWS:-1}"
    OPT_EFFECTS="${STRIKERS_CUTSCENE_EFFECTS:-0}"
    OPT_CULL_EXTRA="${STRIKERS_CUTSCENE_CULL_EXTRA:-1}"
    OPT_MAX_DIST="${STRIKERS_CUTSCENE_MAX_DIST:-85.0}"
    OPT_LOD="${STRIKERS_CUTSCENE_LOD:-0.035}"
    OPT_FAST_TRANS="${STRIKERS_FAST_TRANSITIONS:-1}"
    OPT_NO_SCREENGRAB="${STRIKERS_DISABLE_SCREENGRAB:-1}"
    OPT_NO_OUTLINE="${STRIKERS_DISABLE_TRANSITION_OUTLINE:-1}"
    OPT_CULL_MODELS="${STRIKERS_CULL_EXTRA_MODELS:-1}"
    OPT_MAX_PARTICLES="${STRIKERS_MAX_PARTICLES:-512}"
    OPT_FENCE_SPARKS="${STRIKERS_FENCE_SPARKS:-0}"
    OPT_FENCE_RATE="${STRIKERS_FENCE_RATE:-0.06}"
    OPT_ENABLE_DOF="${STRIKERS_ENABLE_DOF:-0}"
    OPT_ENABLE_REPLAYS="${STRIKERS_ENABLE_REPLAYS:-1}"
  fi

  LANG_FILE="$GAMEDIR/language.env"
  CURR_LANG_NAME="Español (Por defecto)"
  if [ -f "$LANG_FILE" ]; then
    source "$LANG_FILE"
    case "$STRIKERS_LANGUAGE" in
      spanish|es|spa) CURR_LANG_NAME="Español" ;;
      english|uk|en) CURR_LANG_NAME="English" ;;
      french|fra|fr) CURR_LANG_NAME="Français" ;;
      german|deu|de) CURR_LANG_NAME="Deutsch" ;;
      italian|ita|it) CURR_LANG_NAME="Italiano" ;;
      *) CURR_LANG_NAME="${STRIKERS_LANGUAGE:-Español}" ;;
    esac
  fi
}

save_config() {
  cat << EOF > "$ENV_FILE"
# Super Mario Strikers - Configuración de Rendimiento y Gráficos
export STRIKERS_PRESET="$OPT_PRESET"
export STRIKERS_ASPECT="$OPT_ASPECT"
export STRIKERS_RES_SCALE="$OPT_RES_SCALE"
export STRIKERS_CUTSCENE_SKIN="$OPT_SKIN"
export STRIKERS_CUTSCENE_CROWD="$OPT_CROWD"
export STRIKERS_CUTSCENE_SHADOWS="$OPT_SHADOWS"
export STRIKERS_GAMEPLAY_SHADOWS="$OPT_GAMEPLAY_SHADOWS"
export STRIKERS_CUTSCENE_EFFECTS="$OPT_EFFECTS"
export STRIKERS_CUTSCENE_CULL_EXTRA="$OPT_CULL_EXTRA"
export STRIKERS_CUTSCENE_MAX_DIST="$OPT_MAX_DIST"
export STRIKERS_CUTSCENE_LOD="$OPT_LOD"
export STRIKERS_FAST_TRANSITIONS="$OPT_FAST_TRANS"
export STRIKERS_DISABLE_SCREENGRAB="$OPT_NO_SCREENGRAB"
export STRIKERS_DISABLE_TRANSITION_OUTLINE="$OPT_NO_OUTLINE"
export STRIKERS_CULL_EXTRA_MODELS="$OPT_CULL_MODELS"
export STRIKERS_MAX_PARTICLES="$OPT_MAX_PARTICLES"
export STRIKERS_FENCE_SPARKS="$OPT_FENCE_SPARKS"
export STRIKERS_FENCE_RATE="$OPT_FENCE_RATE"
export STRIKERS_ENABLE_DOF="$OPT_ENABLE_DOF"
export STRIKERS_ENABLE_REPLAYS="$OPT_ENABLE_REPLAYS"
EOF
}

apply_preset() {
  case "$1" in
    1)
      # Ninguna (Fidelidad GameCube original)
      OPT_PRESET="Ninguna"
      OPT_RES_SCALE="1.0"
      OPT_SKIN="blend"
      OPT_CROWD="1"
      OPT_SHADOWS="1"
      OPT_GAMEPLAY_SHADOWS="1"
      OPT_EFFECTS="1"
      OPT_CULL_EXTRA="0"
      OPT_MAX_DIST="200.0"
      OPT_LOD="0.0"
      OPT_FAST_TRANS="0"
      OPT_NO_SCREENGRAB="0"
      OPT_NO_OUTLINE="0"
      OPT_CULL_MODELS="0"
      OPT_MAX_PARTICLES="1024"
      OPT_FENCE_SPARKS="1"
      OPT_FENCE_RATE="0.01"
      OPT_ENABLE_DOF="0"
      save_config
      dialog --title "Perfil Aplicado" --msgbox "Se ha aplicado el nivel: NINGUNA (Original)\n\n- Todos los efectos, público y sombras originales activos.\n- Transición original 3D de vidrio roto.\n- Modelos blend en cinemáticas.\n- Escala de render: 1.0x Nativo (480p).\n(Nota: Puede causar caídas de FPS en consolas portátiles)." 12 60 >"$CURR_TTY" 2>&1 <"$CURR_TTY"
      ;;
    2)
      # Bajo (Mínimos recortes, evita cuelgues de vértices)
      OPT_PRESET="Bajo"
      OPT_RES_SCALE="0.90"
      OPT_SKIN="rigid"
      OPT_CROWD="1"
      OPT_SHADOWS="1"
      OPT_GAMEPLAY_SHADOWS="1"
      OPT_EFFECTS="1"
      OPT_CULL_EXTRA="1"
      OPT_MAX_DIST="120.0"
      OPT_LOD="0.01"
      OPT_FAST_TRANS="1"
      OPT_NO_SCREENGRAB="1"
      OPT_NO_OUTLINE="1"
      OPT_CULL_MODELS="1"
      OPT_MAX_PARTICLES="768"
      OPT_FENCE_SPARKS="1"
      OPT_FENCE_RATE="0.03"
      OPT_ENABLE_DOF="0"
      save_config
      dialog --title "Perfil Aplicado" --msgbox "Se ha aplicado el nivel: BAJO\n\n- Mantiene público, sombras y bengalas en cinemáticas.\n- Elimina saturación de vértices en repeticiones.\n- Transición de escena optimizada (Fade suave).\n- Escala de render: 0.90x Alta Calidad (400p)." 11 58 >"$CURR_TTY" 2>&1 <"$CURR_TTY"
      ;;
    3)
      # Medio (Equilibrado / Recomendado)
      OPT_PRESET="Medio"
      OPT_RES_SCALE="0.85"
      OPT_SKIN="rigid"
      OPT_CROWD="0"
      OPT_SHADOWS="0"
      OPT_GAMEPLAY_SHADOWS="1"
      OPT_EFFECTS="0"
      OPT_CULL_EXTRA="1"
      OPT_MAX_DIST="85.0"
      OPT_LOD="0.035"
      OPT_FAST_TRANS="1"
      OPT_NO_SCREENGRAB="1"
      OPT_NO_OUTLINE="1"
      OPT_CULL_MODELS="1"
      OPT_MAX_PARTICLES="512"
      OPT_FENCE_SPARKS="0"
      OPT_FENCE_RATE="0.06"
      OPT_ENABLE_DOF="0"
      save_config
      dialog --title "Perfil Aplicado" --msgbox "Se ha aplicado el nivel: MEDIO [RECOMENDADO]\n\n- Balance ideal de rendimiento y calidad para RK3326.\n- Cinemáticas y repeticiones a 50-60 FPS fluidos.\n- Culling inteligente de estadio y público en cortes.\n- Escala de render: 0.85x Equilibrado (380p)." 11 58 >"$CURR_TTY" 2>&1 <"$CURR_TTY"
      ;;
    4)
      # Alto (Máximo rendimiento)
      OPT_PRESET="Alto"
      OPT_RES_SCALE="0.75"
      OPT_SKIN="rigid"
      OPT_CROWD="0"
      OPT_SHADOWS="0"
      OPT_GAMEPLAY_SHADOWS="0"
      OPT_EFFECTS="0"
      OPT_CULL_EXTRA="1"
      OPT_MAX_DIST="60.0"
      OPT_LOD="0.05"
      OPT_FAST_TRANS="cut"
      OPT_NO_SCREENGRAB="1"
      OPT_NO_OUTLINE="1"
      OPT_CULL_MODELS="1"
      OPT_MAX_PARTICLES="256"
      OPT_FENCE_SPARKS="0"
      OPT_FENCE_RATE="0.10"
      OPT_ENABLE_DOF="0"
      save_config
      dialog --title "Perfil Aplicado" --msgbox "Se ha aplicado el nivel: ALTO (Máximo Rendimiento)\n\n- Sombras generales en juego eliminadas (Mayor fluidez).\n- Cortes de escena instantáneos (sin esperas).\n- Culling agresivo de distancia y partículas reducidas.\n- Escala de render: 0.75x Máxima Fluidez (336p).\n- Máxima tasa de cuadros y ahorro de batería." 12 58 >"$CURR_TTY" 2>&1 <"$CURR_TTY"
      ;;
  esac
}

get_aspect_desc() {
  case "$OPT_ASPECT" in
    4:3|4/3) echo "4:3 (Estándar 640x480)" ;;
    16:9|16/9) echo "16:9 (Panorámica 854x480 / 720p)" ;;
    1:1|1/1) echo "1:1 (Cuadrada 720x720 / RGB30)" ;;
    3:2|3/2) echo "3:2 (GBA / RG351P 480x320)" ;;
    auto|AUTO) echo "Automático (Detectar panel)" ;;
    *) echo "$OPT_ASPECT" ;;
  esac
}

get_res_desc() {
  case "$OPT_RES_SCALE" in
    1.0|1) echo "1.0x Nativo (480p - Calidad óptima)" ;;
    0.90|0.9) echo "0.90x Alta Calidad (400p - Gran nitidez)" ;;
    0.85) echo "0.85x Equilibrado (380p - Recomendado)" ;;
    0.75) echo "0.75x Rendimiento (336p - Máxima fluidez)" ;;
    auto|0|0.0) echo "Automático (Escala a ventana)" ;;
    *) echo "${OPT_RES_SCALE}x" ;;
  esac
}

aspect_menu() {
  local a_choice
  a_choice=$(dialog --output-fd 1 \
    --clear \
    --backtitle "Super Mario Strikers - Formato de Pantalla" \
    --title "Formato de Pantalla (Aspect Ratio)" \
    --menu "Selecciona la relación de aspecto según tu modelo de consola:\n(Actual: $(get_aspect_desc))" 16 65 5 \
    "auto" "Automático (Detecta resolución de pantalla)" \
    "4:3"  "4:3 (Estándar R36S / RG353 / 640x480)" \
    "16:9" "16:9 (Panorámica / Clones R36S Widescreen)" \
    "1:1"  "1:1 (Cuadrada / RGB30 / Cubo 720x720)" \
    "3:2"  "3:2 (GBA / RG351P / 480x320)" \
    2>"$CURR_TTY" <"$CURR_TTY")
  if [ -n "$a_choice" ]; then
    OPT_ASPECT="$a_choice"
    save_config
    dialog --title "Pantalla Configurada" --msgbox "Formato configurado: $(get_aspect_desc).\nSe aplicará al iniciar el juego." 8 55 >"$CURR_TTY" 2>&1 <"$CURR_TTY"
  fi
}

resolution_menu() {
  local r_choice
  r_choice=$(dialog --output-fd 1 \
    --clear \
    --backtitle "Super Mario Strikers - Escala de Renderizado" \
    --title "Escala de Renderizado / Resolución Interna" \
    --menu "Ajusta la resolución interna para optimizar rendimiento:\n(Actual: $(get_res_desc))" 17 68 5 \
    "1.0"  "1.0x Nativo (480p - Calidad óptima y nítida)" \
    "0.90" "0.90x Alta Calidad (400p - Gran nitidez)" \
    "0.85" "0.85x Equilibrado (380p - Recomendado en pantallas 720p)" \
    "0.75" "0.75x Rendimiento (336p - Máxima fluidez de cuadros)" \
    "auto" "Automático (Ajustar a tamaño de pantalla)" \
    2>"$CURR_TTY" <"$CURR_TTY")
  if [ -n "$r_choice" ]; then
    OPT_RES_SCALE="$r_choice"
    save_config
    dialog --title "Resolución Configurada" --msgbox "Escala configurada: $(get_res_desc).\nSe aplicará al iniciar el juego." 8 55 >"$CURR_TTY" 2>&1 <"$CURR_TTY"
  fi
}

show_summary() {
  local msg="Configuración Gráfica Actual:\n\n"
  msg+="• Nivel General: $OPT_PRESET\n"
  msg+="• Formato de Pantalla: $(get_aspect_desc)\n"
  msg+="• Escala de Render: $(get_res_desc)\n"
  msg+="• Sombras en Juego: $([ "$OPT_GAMEPLAY_SHADOWS" = "1" ] && echo "Activadas" || echo "Desactivadas (Rápido)")\n"
  msg+="• Skin Cinemáticas: $([ "$OPT_SKIN" = "rigid" ] && echo "Rigid (Rápido)" || echo "Blend (Original)")\n"
  msg+="• Público Cinemáticas: $([ "$OPT_CROWD" = "1" ] && echo "Visible" || echo "Oculto")\n"
  msg+="• Sombras Cinemáticas: $([ "$OPT_SHADOWS" = "1" ] && echo "Activadas" || echo "Desactivadas")\n"
  msg+="• Flashes/Efectos Cinemáticas: $([ "$OPT_EFFECTS" = "1" ] && echo "Activados" || echo "Desactivados")\n"
  msg+="• Culling Props Estadio: $([ "$OPT_CULL_EXTRA" = "1" ] && echo "Activado" || echo "Desactivado")\n"
  msg+="• Transición Escena: $([ "$OPT_FAST_TRANS" = "1" ] && echo "Fade Suave" || ([ "$OPT_FAST_TRANS" = "cut" ] && echo "Corte Instantáneo" || echo "3D Original"))\n"
  msg+="• Chispas Valla: $([ "$OPT_FENCE_SPARKS" = "1" ] && echo "Activadas" || echo "Desactivadas")\n"
  msg+="• Máx Partículas: $OPT_MAX_PARTICLES\n"
  msg+="• Profundidad de Campo (DOF): $([ "$OPT_ENABLE_DOF" = "1" ] && echo "Activada" || echo "Desactivada")\n"
  msg+="• Repeticiones tras Gol: $([ "$OPT_ENABLE_REPLAYS" = "1" ] && echo "Activadas" || echo "Desactivadas (Solo victoria)")\n"
  dialog --title "Resumen de Configuración" --msgbox "$msg" 20 65 >"$CURR_TTY" 2>&1 <"$CURR_TTY"
}

customize_menu() {
  while true; do
    local st_aspect="$(get_aspect_desc)"
    local st_res="$(get_res_desc)"
    local st_gameplay_shadows="$([ "$OPT_GAMEPLAY_SHADOWS" = "1" ] && echo "Activadas" || echo "Desactivadas (Rápido)")"
    local st_skin="$([ "$OPT_SKIN" = "rigid" ] && echo "Rigid (Rápido)" || echo "Blend (Original)")"
    local st_crowd="$([ "$OPT_CROWD" = "1" ] && echo "Visible" || echo "Oculto (Rápido)")"
    local st_shadows="$([ "$OPT_SHADOWS" = "1" ] && echo "Activadas" || echo "Desactivadas (Rápido)")"
    local st_effects="$([ "$OPT_EFFECTS" = "1" ] && echo "Activados" || echo "Desactivados (Rápido)")"
    local st_cull="$([ "$OPT_CULL_EXTRA" = "1" ] && echo "Activado (Culling)" || echo "Desactivado")"
    local st_trans="$([ "$OPT_FAST_TRANS" = "1" ] && echo "Fade Blanco (Suave)" || ([ "$OPT_FAST_TRANS" = "cut" ] && echo "Corte (Instantáneo)" || echo "3D Original (Pesado)"))"
    local st_sparks="$([ "$OPT_FENCE_SPARKS" = "1" ] && echo "Activadas" || echo "Desactivadas (Rápido)")"
    local st_parts="$OPT_MAX_PARTICLES"
    local st_dof="$([ "$OPT_ENABLE_DOF" = "1" ] && echo "Activado" || echo "Desactivado (Rápido)")"
    local st_replays="$([ "$OPT_ENABLE_REPLAYS" = "1" ] && echo "Activadas" || echo "Desactivadas (Rápido)")"

    local c_item
    c_item=$(dialog --output-fd 1 \
      --clear \
      --backtitle "Super Mario Strikers - Personalización Individual" \
      --title "Opciones Gráficas Individuales" \
      --menu "Selecciona una opción con A para modificarla:" 23 70 14 \
      "1" "Formato de Pantalla: $st_aspect" \
      "2" "Escala de Renderizado: $st_res" \
      "3" "Sombras en Juego (Gameplay): $st_gameplay_shadows" \
      "4" "Skin en Cinemáticas: $st_skin" \
      "5" "Público en Cinemáticas: $st_crowd" \
      "6" "Sombras en Cinemáticas: $st_shadows" \
      "7" "Efectos/Flashes Cinemáticas: $st_effects" \
      "8" "Culling Props Estadio: $st_cull" \
      "9" "Transición de Escena: $st_trans" \
      "10" "Chispas en Valla Eléctrica: $st_sparks" \
      "11" "Límite de Partículas: $st_parts" \
      "12" "Profundidad de Campo (DOF): $st_dof" \
      "13" "Repeticiones tras Gol: $st_replays" \
      "14" "<-- Volver al menú principal" \
      2>"$CURR_TTY" <"$CURR_TTY")

    case "$c_item" in
      1)
        aspect_menu
        OPT_PRESET="Personalizado"
        ;;
      2)
        resolution_menu
        OPT_PRESET="Personalizado"
        ;;
      3)
        if [ "$OPT_GAMEPLAY_SHADOWS" = "1" ]; then OPT_GAMEPLAY_SHADOWS="0"; else OPT_GAMEPLAY_SHADOWS="1"; fi
        OPT_PRESET="Personalizado"
        save_config
        ;;
      4)
        if [ "$OPT_SKIN" = "rigid" ]; then OPT_SKIN="blend"; else OPT_SKIN="rigid"; fi
        OPT_PRESET="Personalizado"
        save_config
        ;;
      5)
        if [ "$OPT_CROWD" = "1" ]; then OPT_CROWD="0"; else OPT_CROWD="1"; fi
        OPT_PRESET="Personalizado"
        save_config
        ;;
      6)
        if [ "$OPT_SHADOWS" = "1" ]; then OPT_SHADOWS="0"; else OPT_SHADOWS="1"; fi
        OPT_PRESET="Personalizado"
        save_config
        ;;
      7)
        if [ "$OPT_EFFECTS" = "1" ]; then OPT_EFFECTS="0"; else OPT_EFFECTS="1"; fi
        OPT_PRESET="Personalizado"
        save_config
        ;;
      8)
        if [ "$OPT_CULL_EXTRA" = "1" ]; then OPT_CULL_EXTRA="0"; else OPT_CULL_EXTRA="1"; fi
        OPT_PRESET="Personalizado"
        save_config
        ;;
      9)
        local t_choice
        t_choice=$(dialog --output-fd 1 \
          --clear \
          --title "Tipo de Transición" \
          --menu "Selecciona el método de transición de escena:" 12 55 3 \
          "1" "Fade Blanco Suave (Rápido y estético)" \
          "2" "Corte Instantáneo (Máxima velocidad)" \
          "3" "3D Original (Efecto vidrio roto pesado)" \
          2>"$CURR_TTY" <"$CURR_TTY")
        case "$t_choice" in
          1) OPT_FAST_TRANS="1" ;;
          2) OPT_FAST_TRANS="cut" ;;
          3) OPT_FAST_TRANS="0" ;;
        esac
        OPT_PRESET="Personalizado"
        save_config
        ;;
      10)
        if [ "$OPT_FENCE_SPARKS" = "1" ]; then OPT_FENCE_SPARKS="0"; else OPT_FENCE_SPARKS="1"; fi
        OPT_PRESET="Personalizado"
        save_config
        ;;
      11)
        local p_choice
        p_choice=$(dialog --output-fd 1 \
          --clear \
          --title "Límite de Partículas" \
          --menu "Selecciona el límite máximo de partículas en pantalla:" 13 55 4 \
          "256" "Bajo (256 partículas - Máximo rendimiento)" \
          "512" "Medio (512 partículas - Recomendado)" \
          "768" "Alto (768 partículas - Más efectos)" \
          "1024" "Ultra (1024 partículas - Original)" \
          2>"$CURR_TTY" <"$CURR_TTY")
        if [ -n "$p_choice" ]; then
          OPT_MAX_PARTICLES="$p_choice"
          OPT_PRESET="Personalizado"
          save_config
        fi
        ;;
      12)
        if [ "$OPT_ENABLE_DOF" = "1" ]; then OPT_ENABLE_DOF="0"; else OPT_ENABLE_DOF="1"; fi
        OPT_PRESET="Personalizado"
        save_config
        ;;
      13)
        if [ "$OPT_ENABLE_REPLAYS" = "1" ]; then OPT_ENABLE_REPLAYS="0"; else OPT_ENABLE_REPLAYS="1"; fi
        save_config
        ;;
      14|*)
        break
        ;;
    esac
  done
}

load_config

while true; do
  CHOICE=$(dialog --output-fd 1 \
    --clear \
    --backtitle "Super Mario Strikers - Configuración" \
    --title "Optimizaciones y Opciones" \
    --menu "Nivel: $OPT_PRESET | Pantalla: $(get_aspect_desc) | Idioma: $CURR_LANG_NAME\n\nNavega con la cruceta o stick y pulsa A:" 20 72 8 \
    "1" "Seleccionar Nivel General (Ninguna / Bajo / Medio / Alto)" \
    "2" "Formato de Pantalla (4:3 / 16:9 / 1:1 / 3:2 / Auto)" \
    "3" "Escala de Renderizado (1.0x / 0.85x / 0.75x / Auto)" \
    "4" "Personalizar Opciones Individuales de Rendimiento" \
    "5" "Seleccionar Idioma ROM Europea (Español / English / etc.)" \
    "6" "Ver Resumen de Opciones Activas" \
    "7" "Restaurar por Defecto (Medio / Auto / Español)" \
    "8" "Guardar y Salir" \
    2>"$CURR_TTY" <"$CURR_TTY")

  case "$CHOICE" in
    1)
      P_CHOICE=$(dialog --output-fd 1 \
        --clear \
        --title "Niveles Generales de Optimización" \
        --menu "Selecciona la agresividad de las optimizaciones:" 14 62 4 \
        "1" "Ninguna (Original GameCube - Sin recortes)" \
        "2" "Bajo (Calidad alta, corrige tirones de vértices)" \
        "3" "Medio [RECOMENDADO] (Óptimo para RK3326)" \
        "4" "Alto (Máxima fluidez y corte instantáneo)" \
        2>"$CURR_TTY" <"$CURR_TTY")
      if [ -n "$P_CHOICE" ]; then
        apply_preset "$P_CHOICE"
        load_config
      fi
      ;;
    2)
      aspect_menu
      load_config
      ;;
    3)
      resolution_menu
      load_config
      ;;
    4)
      customize_menu
      load_config
      ;;
    5)
      L_CHOICE=$(dialog --output-fd 1 \
        --clear \
        --title "Selección de Idioma" \
        --menu "Selecciona el idioma para versión Europea (G4QP01) o Japonesa:" 15 65 5 \
        "1" "Español (Castellano / Spanish)" \
        "2" "English (UK / US English)" \
        "3" "Français (French)" \
        "4" "Deutsch (German)" \
        "5" "Italiano (Italian)" \
        2>"$CURR_TTY" <"$CURR_TTY")
      case "$L_CHOICE" in
        1)
          echo 'export STRIKERS_LANGUAGE="spanish"' > "$LANG_FILE"
          dialog --title "Idioma Guardado" --msgbox "Idioma configurado: Español.\nSe aplicará automáticamente al iniciar el juego." 8 55 >"$CURR_TTY" 2>&1 <"$CURR_TTY"
          ;;
        2)
          echo 'export STRIKERS_LANGUAGE="english"' > "$LANG_FILE"
          dialog --title "Idioma Guardado" --msgbox "Language set to: English.\nWill apply on next game launch." 8 55 >"$CURR_TTY" 2>&1 <"$CURR_TTY"
          ;;
        3)
          echo 'export STRIKERS_LANGUAGE="french"' > "$LANG_FILE"
          dialog --title "Idioma Guardado" --msgbox "Langue configurée: Français.\nS'appliquera au prochain lancement." 8 55 >"$CURR_TTY" 2>&1 <"$CURR_TTY"
          ;;
        4)
          echo 'export STRIKERS_LANGUAGE="german"' > "$LANG_FILE"
          dialog --title "Idioma Guardado" --msgbox "Sprache eingestellt: Deutsch.\nWird beim nächsten Start angewendet." 8 55 >"$CURR_TTY" 2>&1 <"$CURR_TTY"
          ;;
        5)
          echo 'export STRIKERS_LANGUAGE="italian"' > "$LANG_FILE"
          dialog --title "Idioma Guardado" --msgbox "Lingua impostata: Italiano.\nSi applicherà al próximo avvio." 8 55 >"$CURR_TTY" 2>&1 <"$CURR_TTY"
          ;;
      esac
      load_config
      ;;
    6)
      show_summary
      ;;
    7)
      rm -f "$ENV_FILE" "$LANG_FILE"
      load_config
      dialog --title "Restaurado" --msgbox "Se ha restaurado la configuración por defecto (Medio / Auto / Español)." 7 55 >"$CURR_TTY" 2>&1 <"$CURR_TTY"
      ;;
    8|*)
      break
      ;;
  esac
done

exit 0
