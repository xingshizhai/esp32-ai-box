#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd -L "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -L)"
product="${1:-}"
action="${2:-build}"
port="${3:-}"

case "$product" in
  ai-pet)
    project_dir="$repo_dir/ai-pet"
    default_port="/dev/ttyACM0"
    ;;
  anti-pet)
    project_dir="$repo_dir/anti-pet"
    default_port="/dev/ttyUSB0"
    ;;
  *)
    echo "Usage: $0 {ai-pet|anti-pet} [build|flash|monitor|menuconfig] [port]" >&2
    exit 2
    ;;
esac

cd "$project_dir"
case "$action" in
  build|menuconfig|fullclean)
    idf.py "$action"
    ;;
  flash|monitor)
    idf.py -p "${port:-$default_port}" "$action"
    ;;
  *)
    echo "Unsupported action: $action" >&2
    exit 2
    ;;
esac
