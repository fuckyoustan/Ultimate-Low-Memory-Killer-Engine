# Don't modify anything after this
if [ -f $INFO ]; then
  while read LINE; do
    if [ "$(echo -n $LINE | tail -c 1)" == "~" ]; then
      continue
    elif [ -f "$LINE~" ]; then
      mv -f $LINE~ $LINE
    else
      rm -f $LINE
      while true; do
        LINE=$(dirname $LINE)
        [ "$(ls -A $LINE 2>/dev/null)" ] && break 1 || rm -rf $LINE
      done
    fi
  done < $INFO
  rm -f $INFO
fi
SWAP_FILE=/data/lmk_swap
if swapon --show 2>/dev/null | grep -q "$SWAP_FILE"; then
    swapoff "$SWAP_FILE" 2>/dev/null
    [ -f "$SWAP_FILE" ] && rm -f "$SWAP_FILE"
fi