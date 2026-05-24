#!/bin/bash
# Sincronizador de brillo de teclado Chuwi - v12 (Con desactivación de temporizador)
FILE="/sys/class/leds/minibook::kbd_backlight/brightness"
TIMER="/sys/class/leds/minibook::kbd_backlight/auto_off_timer"

# 1. Esperar a que el driver cargue los archivos
while [ ! -f "$FILE" ]; do sleep 2; done

# 2. Desactivar el temporizador de la BIOS (0 = siempre encendido)
if [ -f "$TIMER" ]; then
    echo 0 > "$TIMER" 2>/dev/null
fi

# 3. Memoria de estado inicial
LAST_VAL=$(cat "$FILE")

while true; do
    if [ -f "$FILE" ]; then
        CURRENT_VAL=$(cat "$FILE")
        
        if [ "$CURRENT_VAL" != "$LAST_VAL" ]; then
            # Sincronización matemática (0-255 -> 0-100)
            PERCENTAGE=$(( ($CURRENT_VAL * 100) / 255 ))
            
            # Mandamos la orden a la sesión de GNOME
            gdbus call --session --dest org.gnome.SettingsDaemon.Power \
            --object-path /org/gnome/SettingsDaemon/Power \
            --method org.freedesktop.DBus.Properties.Set \
            "org.gnome.SettingsDaemon.Power.Keyboard" "Brightness" "<int32 $PERCENTAGE>"
            
            LAST_VAL=$CURRENT_VAL
        fi
    fi
    sleep 0.2
done
