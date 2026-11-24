#!/system/bin/sh
MODDIR=${0%/*}

while [ -z "$(getprop sys.boot_completed)" ]; do
    sleep 5
done

sleep 2
lmk_engine --start