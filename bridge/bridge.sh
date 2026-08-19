# MicScribe `bridge` command. Sourced from ~/.bashrc.
#
#   bridge run [flags]   start the bridge (any bridge.py flag passes through)
#   bridge --type        flags with no subcommand imply `run`
#   bridge stop          release the serial port
#   bridge status        show what currently holds the port
#
# The board allows only one open connection at a time, so `bridge run` stops an
# existing instance first instead of failing with a confusing busy error.

# Walk up from $PWD to find the checkout, so this works from the repo, from
# bridge/, or from anywhere if the checkout sits at the usual place.
_micscribe_root() {
  local dir="$PWD"
  while [ "$dir" != "/" ] && [ -n "$dir" ]; do
    if [ -f "$dir/bridge/bridge.py" ]; then
      printf '%s\n' "$dir"
      return 0
    fi
    dir="$(dirname "$dir")"
  done
  if [ -f "$HOME/mic_esp/bridge/bridge.py" ]; then
    printf '%s\n' "$HOME/mic_esp"
    return 0
  fi
  return 1
}

bridge() {
  local root py
  root="$(_micscribe_root)" || {
    echo "bridge: no MicScribe checkout found here or at ~/mic_esp" >&2
    return 1
  }

  py="$root/.venv/bin/python"
  if [ ! -x "$py" ]; then
    echo "bridge: venv missing. Create it with:" >&2
    echo "  python3 -m venv '$root/.venv' && '$root/.venv/bin/pip' install pyserial" >&2
    return 1
  fi

  case "${1:-run}" in
    stop)
      if pkill -f "bridge/bridge.py" 2>/dev/null; then
        echo "bridge: stopped"
      else
        echo "bridge: nothing running"
      fi
      return 0
      ;;
    status)
      local port
      port=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
      if [ -z "$port" ]; then
        echo "bridge: no board plugged in"
        return 1
      fi
      echo "bridge: board at $port"
      if lsof "$port" 2>/dev/null | tail -n +2 | grep -q .; then
        lsof "$port" 2>/dev/null
      else
        echo "bridge: port is free"
      fi
      return 0
      ;;
    help)
      echo "usage: bridge [run|stop|status] [flags]"
      echo "  bridge run --type      type transcripts into the focused app"
      echo "  bridge run --copy      copy transcripts to the clipboard"
      echo "  bridge run -v          also show the device log lines"
      echo "  bridge --help          full flag list from bridge.py"
      return 0
      ;;
    run)
      shift
      ;;
    -*)
      # bare flags mean `run`, so `bridge --type` works
      ;;
    *)
      echo "bridge: unknown command '$1' (try: bridge help)" >&2
      return 1
      ;;
  esac

  # Only one process may hold the serial port, so clear any stale instance.
  if pgrep -f "bridge/bridge.py" >/dev/null 2>&1; then
    echo "bridge: stopping the instance that already holds the port"
    pkill -f "bridge/bridge.py" 2>/dev/null
    sleep 1
  fi

  "$py" "$root/bridge/bridge.py" "$@"
}
