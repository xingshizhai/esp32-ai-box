# ESP32 CJK font asset

`generate_cjk_font.py` generates the seekable `CJKF` bitmap used by the UI.
It covers GB2312, the static UI strings, and common Chinese punctuation. The
firmware validates the header/index, keeps the index and a 128-glyph LRU cache
in PSRAM, and reads A2 glyph bitmaps from SPIFFS on demand.

Regenerate the checked-in 14 px asset after changing the source font:

```sh
tools/venv/bin/python tools/fonts/generate_cjk_font.py \
  --font managed_components/lvgl__lvgl/scripts/built_in_font/SourceHanSansSC-Normal.otf \
  --output assets/spiffs_storage/font_zh_gb2312_14.cjkf \
  --size 14
```

On the device UART console, `font test` displays a fixed Chinese coverage
sample and `font stats` prints cache hits, misses, IO errors, and missing
Unicode glyphs.
