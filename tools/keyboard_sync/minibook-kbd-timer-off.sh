#!/bin/bash
# Esperar hasta que el archivo del temporizador aparezca
while [ ! -f /sys/class/leds/minibook::kbd_backlight/auto_off_timer ]; do
  sleep 2
done
# Apagar el temporizador de la BIOS (0 = Siempre encendido)
echo 0 > /sys/class/leds/minibook::kbd_backlight/auto_off_timer
