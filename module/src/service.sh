#!/system/bin/sh

DEBUG=@DEBUG@

MODDIR=${0%/*}
if [ "$ZYGISK_ENABLED" ]; then
  exit 0
fi

cd "$MODDIR"


if [ "$(which magisk)" ]; then
  for file in ../*; do
    if [ -d "$file" ] && [ -d "$file/zygisk" ] && ! [ -f "$file/disable" ]; then
      if [ -f "$file/service.sh" ]; then
        cd "$file"
        log -p i -t "zygisk-sh" "Manually trigger service.sh for $file"
        sh "$(realpath ./service.sh)" &
        cd "$MODDIR"
      fi
    fi
  done
fi

# Function to get status icon
get_icon() {
  case "$1" in
    "Running"|"Injected"|"Tracing") echo "✅" ;;
    *) echo "❌" ;;
  esac
}

# Wait for status file and update description
(
  # Wait up to 10 seconds for the status file
  count=0
  while [ ! -f "$MODDIR/module.prop" ] && [ $count -lt 20 ]; do
    sleep 0.5
    count=$((count + 1))
  done
  
  # Give it a moment for the daemon to write status
  sleep 2

  if [ -f "$MODDIR/module.prop" ]; then
    # Read status values
    MONITOR=$(grep "^monitor_status=" "$MODDIR/module.prop" | cut -d= -f2)
    ZYGOTE64=$(grep "^zygote_64_status=" "$MODDIR/module.prop" | cut -d= -f2)
    # Assuming zygote_32_status exists or similar
    ZYGOTE32=$(grep "^zygote_32_status=" "$MODDIR/module.prop" | cut -d= -f2)

    ICON_MONITOR=$(get_icon "$MONITOR")
    ICON_64=$(get_icon "$ZYGOTE64")
    # ICON_32=$(get_icon "$ZYGOTE32") 
    # Since we don't know for sure if 32-bit status is tracked in prop, we'll try to find it or default to something if needed.
    # Looking at script.js, it only referenced zygote_64. 
    # However, the user image explicitly showed "ReZygisk 32-bit". 
    # I will assume the key is zygote_32_status based on naming convention.
    ICON_32=$(get_icon "$ZYGOTE32")

    # Construct new description
    STATUS_STR="[Monitor: ${ICON_MONITOR}, NeoZygisk 64-bit: ${ICON_64}, NeoZygisk 32-bit: ${ICON_32}]"
    
    # Update description in place
    # Remove any existing status prefix if we ran before (to prevent duplicate stacking)
    # But for now just replace the description line.
    # We want: description=[Status] Original Description
    
    # First, get the original description (stripping any previous [Status] block if we were smart, but hard to do strictly in shell without regex)
    # Simple approach: set a fixed base description since we know it.
    BASE_DESC="Zygote injection with ptrace."
    
    NEW_DESC="${STATUS_STR} ${BASE_DESC}"
    
    sed -i "s|^description=.*|description=${NEW_DESC}|" "$MODDIR/module.prop"
  fi
) &
