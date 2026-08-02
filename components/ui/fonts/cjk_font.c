#include "cjk_font.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#define CJK_FONT_MAGIC 0x464B4A43u /* "CJKF" as little-endian uint32 */
#define CJK_FONT_VERSION 1u
#define CJK_FONT_CACHE_SLOTS 128u
#define CJK_FONT_MAX_BITMAP 256u

static const char *TAG = "cjk_font";

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint32_t glyph_count;
    uint32_t index_offset;
    uint32_t data_offset;
    uint32_t line_height;
    uint32_t base_line;
    uint32_t max_bitmap;
} cjk_header_t;

typedef struct __attribute__((packed)) {
    uint32_t codepoint;
    uint32_t offset;
    uint16_t length;
    uint8_t advance;
    uint8_t width;
    uint8_t height;
    int8_t ofs_x;
    int8_t ofs_y;
    uint8_t reserved;
} cjk_entry_t;

typedef struct {
    uint32_t glyph_index;
    uint32_t stamp;
    uint16_t length;
    bool valid;
    uint8_t bitmap[CJK_FONT_MAX_BITMAP];
} cjk_cache_slot_t;

typedef struct {
    FILE *file;
    size_t file_size;
    cjk_entry_t *entries;
    uint32_t glyph_count;
    uint32_t clock;
    cjk_cache_slot_t *cache;
    uint32_t cache_hits;
    uint32_t cache_misses;
    uint32_t io_errors;
    uint32_t missing_glyphs;
} cjk_font_dsc_t;

static bool find_glyph(const cjk_font_dsc_t *dsc, uint32_t codepoint, uint32_t *index)
{
    uint32_t lo = 0;
    uint32_t hi = dsc->glyph_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t value = dsc->entries[mid].codepoint;
        if (value < codepoint) lo = mid + 1;
        else hi = mid;
    }
    if (lo >= dsc->glyph_count || dsc->entries[lo].codepoint != codepoint) return false;
    *index = lo;
    return true;
}

static bool get_glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *out,
                          uint32_t letter, uint32_t letter_next)
{
    (void)letter_next;
    cjk_font_dsc_t *dsc = (cjk_font_dsc_t *)font->dsc;
    uint32_t index;
    if (!find_glyph(dsc, letter, &index)) {
        /* ASCII is intentionally handled by the compact compiled fallback. */
        if (letter >= 0x80) {
            dsc->missing_glyphs++;
            if (dsc->missing_glyphs <= 8 || (dsc->missing_glyphs % 100) == 0) {
                ESP_LOGW(TAG, "missing glyph U+%04lX (count=%lu)",
                         (unsigned long)letter, (unsigned long)dsc->missing_glyphs);
            }
        }
        return false;
    }
    const cjk_entry_t *entry = &dsc->entries[index];
    out->adv_w = entry->advance;
    out->box_w = entry->width;
    out->box_h = entry->height;
    out->ofs_x = entry->ofs_x;
    out->ofs_y = entry->ofs_y;
    out->stride = (entry->width + 3u) / 4u;
    out->format = LV_FONT_GLYPH_FORMAT_A2;
    out->is_placeholder = false;
    out->gid.index = index;
    return true;
}

static const void *get_glyph_bitmap(lv_font_glyph_dsc_t *glyph, lv_draw_buf_t *draw_buf)
{
    (void)draw_buf;
    cjk_font_dsc_t *dsc = (cjk_font_dsc_t *)glyph->resolved_font->dsc;
    uint32_t index = glyph->gid.index;
    if (index >= dsc->glyph_count) return NULL;

    cjk_cache_slot_t *victim = NULL;
    for (uint32_t i = 0; i < CJK_FONT_CACHE_SLOTS; ++i) {
        cjk_cache_slot_t *slot = &dsc->cache[i];
        if (slot->valid && slot->glyph_index == index) {
            slot->stamp = ++dsc->clock;
            dsc->cache_hits++;
            if (glyph->req_raw_bitmap) return slot->bitmap;
            uint8_t *out = draw_buf->data;
            uint32_t out_stride = draw_buf->header.stride;
            for (uint32_t y = 0; y < glyph->box_h; ++y) {
                const uint8_t *in = slot->bitmap + y * glyph->stride;
                for (uint32_t x = 0; x < glyph->box_w; ++x) {
                    out[y * out_stride + x] = (uint8_t)(((in[x / 4] >> (6 - (x % 4) * 2)) & 3) * 85);
                }
            }
            lv_draw_buf_flush_cache(draw_buf, NULL);
            return draw_buf;
        }
        if (!victim || !slot->valid || slot->stamp < victim->stamp) victim = slot;
    }

    const cjk_entry_t *entry = &dsc->entries[index];
    if (entry->length > CJK_FONT_MAX_BITMAP ||
        entry->offset > dsc->file_size || entry->length > dsc->file_size - entry->offset ||
        fseek(dsc->file, (long)entry->offset, SEEK_SET) != 0 ||
        fread(victim->bitmap, 1, entry->length, dsc->file) != entry->length) {
        dsc->io_errors++;
        ESP_LOGE(TAG, "glyph read failed index=%lu errno=%d", (unsigned long)index, errno);
        return NULL;
    }
    victim->glyph_index = index;
    victim->length = entry->length;
    victim->stamp = ++dsc->clock;
    victim->valid = true;
    dsc->cache_misses++;
    if (glyph->req_raw_bitmap) return victim->bitmap;
    uint8_t *out = draw_buf->data;
    uint32_t out_stride = draw_buf->header.stride;
    for (uint32_t y = 0; y < glyph->box_h; ++y) {
        const uint8_t *in = victim->bitmap + y * glyph->stride;
        for (uint32_t x = 0; x < glyph->box_w; ++x) {
            out[y * out_stride + x] = (uint8_t)(((in[x / 4] >> (6 - (x % 4) * 2)) & 3) * 85);
        }
    }
    lv_draw_buf_flush_cache(draw_buf, NULL);
    return draw_buf;
}

lv_font_t *cjk_font_create(const char *path, const lv_font_t *fallback)
{
    struct stat st;
    if (!path || stat(path, &st) != 0 || st.st_size < (off_t)sizeof(cjk_header_t)) return NULL;
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;

    cjk_header_t header;
    if (fread(&header, 1, sizeof(header), file) != sizeof(header) ||
        header.magic != CJK_FONT_MAGIC || header.version != CJK_FONT_VERSION ||
        header.header_size != sizeof(header) || header.glyph_count == 0 ||
        header.glyph_count > 20000 || header.index_offset != sizeof(header) ||
        header.data_offset != header.index_offset + header.glyph_count * sizeof(cjk_entry_t) ||
        header.data_offset > (uint32_t)st.st_size || header.line_height > 64 ||
        header.base_line > header.line_height || header.max_bitmap > CJK_FONT_MAX_BITMAP) {
        ESP_LOGE(TAG, "invalid CJKF header: %s", path);
        fclose(file);
        return NULL;
    }

    cjk_font_dsc_t *dsc = heap_caps_calloc(1, sizeof(*dsc), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    lv_font_t *font = calloc(1, sizeof(*font));
    size_t index_bytes = header.glyph_count * sizeof(cjk_entry_t);
    cjk_entry_t *entries = heap_caps_malloc(index_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    cjk_cache_slot_t *cache = heap_caps_calloc(CJK_FONT_CACHE_SLOTS, sizeof(*cache),
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!dsc || !font || !entries || !cache ||
        fseek(file, (long)header.index_offset, SEEK_SET) != 0 ||
        fread(entries, 1, index_bytes, file) != index_bytes) {
        ESP_LOGE(TAG, "CJKF allocation/index read failed");
        heap_caps_free(cache);
        heap_caps_free(entries);
        heap_caps_free(dsc);
        free(font);
        fclose(file);
        return NULL;
    }

    for (uint32_t i = 0; i < header.glyph_count; ++i) {
        const cjk_entry_t *entry = &entries[i];
        if ((i && entries[i - 1].codepoint >= entry->codepoint) ||
            entry->length > CJK_FONT_MAX_BITMAP || entry->offset < header.data_offset ||
            entry->offset > (uint32_t)st.st_size || entry->length > (uint32_t)st.st_size - entry->offset) {
            ESP_LOGE(TAG, "invalid CJKF index entry %lu", (unsigned long)i);
            heap_caps_free(cache);
            heap_caps_free(entries);
            heap_caps_free(dsc);
            free(font);
            fclose(file);
            return NULL;
        }
    }

    dsc->file = file;
    dsc->file_size = st.st_size;
    dsc->entries = entries;
    dsc->glyph_count = header.glyph_count;
    dsc->cache = cache;
    font->get_glyph_dsc = get_glyph_dsc;
    font->get_glyph_bitmap = get_glyph_bitmap;
    font->line_height = header.line_height;
    font->base_line = header.base_line;
    font->static_bitmap = 1;
    font->dsc = dsc;
    font->fallback = fallback;
    font->underline_position = -2;
    font->underline_thickness = 1;
    ESP_LOGI(TAG, "loaded %lu glyphs, index=%u cache=%u from %s",
             (unsigned long)header.glyph_count, (unsigned)index_bytes,
             (unsigned)(CJK_FONT_CACHE_SLOTS * sizeof(cjk_cache_slot_t)), path);
    return font;
}

void cjk_font_destroy(lv_font_t *font)
{
    if (!font) return;
    cjk_font_dsc_t *dsc = (cjk_font_dsc_t *)font->dsc;
    if (dsc) {
        if (dsc->file) fclose(dsc->file);
        heap_caps_free(dsc->cache);
        heap_caps_free(dsc->entries);
        heap_caps_free(dsc);
    }
    free(font);
}

void cjk_font_log_stats(const lv_font_t *font)
{
    if (!font || !font->dsc) return;
    const cjk_font_dsc_t *dsc = (const cjk_font_dsc_t *)font->dsc;
    ESP_LOGI(TAG, "stats hit=%lu miss=%lu io_err=%lu missing=%lu",
             (unsigned long)dsc->cache_hits, (unsigned long)dsc->cache_misses,
             (unsigned long)dsc->io_errors, (unsigned long)dsc->missing_glyphs);
}
