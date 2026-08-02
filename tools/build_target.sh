#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd -L "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -L)"
target="${1:-}"
action="${2:-build}"
port="${3:-}"

case "$target" in
  box3-ai-pet)
    board="esp32s3_box3"
    role="ai_pet"
    board_symbol="CONFIG_APP_BOARD_ESP32S3_BOX3=y"
    role_symbol="CONFIG_APP_ROLE_AI_PET=y"
    default_port="/dev/ttyACM0"
    ;;
  lcd-ev-anti-pet)
    board="esp32s3_lcd_ev_board"
    role="anti_ai_pet"
    board_symbol="CONFIG_APP_BOARD_ESP32S3_LCD_EV_BOARD=y"
    role_symbol="CONFIG_APP_ROLE_ANTI_AI_PET=y"
    default_port="/dev/ttyUSB0"
    ;;
  *)
    echo "Usage: $0 {box3-ai-pet|lcd-ev-anti-pet} [build|flash|monitor|menuconfig] [port]" >&2
    exit 2
    ;;
esac

target_dir="$repo_dir/targets/$target"
sdkconfig_file="$target_dir/sdkconfig"
build_dir="$repo_dir/build-$target"
mkdir -p "$target_dir"

# Seed credentials and shared product settings from the developer's current
# (git-ignored) sdkconfig once. Each target then evolves independently.
if [[ ! -f "$sdkconfig_file" ]]; then
  if [[ -f "$repo_dir/sdkconfig" ]]; then
    cp "$repo_dir/sdkconfig" "$sdkconfig_file"
  else
    : > "$sdkconfig_file"
  fi
fi

# Board and role are independent dimensions. Normalize the copied config so a
# previous menuconfig selection cannot leak into this target.
sed -i \
  -e '/^CONFIG_APP_BOARD_ESP32S3_BOX3=/d' \
  -e '/^CONFIG_APP_BOARD_ESP32S3_LCD_EV_BOARD=/d' \
  -e '/^CONFIG_APP_BOARD_CUSTOM=/d' \
  -e '/^# CONFIG_APP_BOARD_.* is not set$/d' \
  -e '/^CONFIG_APP_ROLE_AI_PET=/d' \
  -e '/^CONFIG_APP_ROLE_ANTI_AI_PET=/d' \
  -e '/^# CONFIG_APP_ROLE_.* is not set$/d' \
  "$sdkconfig_file"
printf '\n%s\n%s\n' "$board_symbol" "$role_symbol" >> "$sdkconfig_file"

idf_args=(
  -B "$build_dir"
  -D "APP_BOARD=$board"
  -D "APP_ROLE=$role"
  -D "SDKCONFIG=$sdkconfig_file"
)

case "$action" in
  build|menuconfig)
    idf.py "${idf_args[@]}" "$action"
    ;;
  flash|monitor)
    idf.py "${idf_args[@]}" -p "${port:-$default_port}" "$action"
    ;;
  *)
    echo "Unsupported action: $action" >&2
    exit 2
    ;;
esac
