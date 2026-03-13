#include "storage.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "driver/sdmmc_host.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "soc/soc_caps.h"

static const char *TAG = "storage";

#define STORAGE_SD_MOUNT_POINT      "/sdcard"
#define STORAGE_MAX_FILES           (8)
#define STORAGE_ALLOC_UNIT          (16 * 1024)

/*
 * SPI-mode-like SDMMC using different GPIO pins to avoid conflicts
 * Using GPIO 11, 12, 13, 14 instead of 35-38 (which are shared with PMOD)
 */
#define STORAGE_SDMMC_CLK_GPIO      (GPIO_NUM_11)
#define STORAGE_SDMMC_CMD_GPIO      (GPIO_NUM_12)
#define STORAGE_SDMMC_D0_GPIO       (GPIO_NUM_13)
#define STORAGE_SDMMC_D1_GPIO       (GPIO_NUM_14)
#define STORAGE_SDMMC_D2_GPIO       (GPIO_NUM_9)
#define STORAGE_SDMMC_D3_GPIO       (GPIO_NUM_10)

static sdmmc_card_t *s_card = NULL;

static bool storage_has_mp3_extension(const char *file_name)
{
    const char *ext = strrchr(file_name, '.');
    return (ext != NULL) && (strcasecmp(ext, ".mp3") == 0);
}

static esp_err_t storage_find_first_mp3_in_dir(const char *dir_path, char *out_path, size_t out_path_len)
{
    DIR *dir = opendir(dir_path);
    if (dir == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if ((strcmp(entry->d_name, ".") == 0) || (strcmp(entry->d_name, "..") == 0)) {
            continue;
        }
        if (!storage_has_mp3_extension(entry->d_name)) {
            continue;
        }

        char candidate[256] = {0};
        int written = snprintf(candidate, sizeof(candidate), "%s/%s", dir_path, entry->d_name);
        if ((written <= 0) || ((size_t)written >= sizeof(candidate))) {
            continue;
        }

        struct stat st = {0};
        if ((stat(candidate, &st) == 0) && S_ISREG(st.st_mode)) {
            written = snprintf(out_path, out_path_len, "%s", candidate);
            if ((written <= 0) || ((size_t)written >= out_path_len)) {
                closedir(dir);
                return ESP_ERR_INVALID_SIZE;
            }
            closedir(dir);
            return ESP_OK;
        }
    }

    closedir(dir);
    return ESP_ERR_NOT_FOUND;
}

bool storage_sdcard_is_mounted(void)
{
    return (s_card != NULL);
}

const char *storage_sdcard_get_mount_point(void)
{
    return STORAGE_SD_MOUNT_POINT;
}

esp_err_t storage_sdcard_mount(void)
{
    if (s_card != NULL) {
        return ESP_OK;
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4;  // Use 4-bit mode with new GPIO pins

#if SOC_SDMMC_USE_GPIO_MATRIX
    slot_config.clk = STORAGE_SDMMC_CLK_GPIO;
    slot_config.cmd = STORAGE_SDMMC_CMD_GPIO;
    slot_config.d0 = STORAGE_SDMMC_D0_GPIO;
    slot_config.d1 = STORAGE_SDMMC_D1_GPIO;
    slot_config.d2 = STORAGE_SDMMC_D2_GPIO;
    slot_config.d3 = STORAGE_SDMMC_D3_GPIO;
    
    slot_config.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
#else
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
#endif

    slot_config.cd = GPIO_NUM_NC;

    const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = STORAGE_MAX_FILES,
        .allocation_unit_size = STORAGE_ALLOC_UNIT,
    };

    ESP_LOGD(TAG, "Mounting SD card in 4-bit mode: CLK=%d CMD=%d D0=%d D1=%d D2=%d D3=%d",
             STORAGE_SDMMC_CLK_GPIO, STORAGE_SDMMC_CMD_GPIO, 
             STORAGE_SDMMC_D0_GPIO, STORAGE_SDMMC_D1_GPIO,
             STORAGE_SDMMC_D2_GPIO, STORAGE_SDMMC_D3_GPIO);

    esp_err_t err = esp_vfs_fat_sdmmc_mount(STORAGE_SD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD card mount failed (%s)", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "SD card mounted at %s", STORAGE_SD_MOUNT_POINT);
    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}

esp_err_t storage_sdcard_unmount(void)
{
    if (s_card == NULL) {
        return ESP_OK;
    }

    esp_err_t err = esp_vfs_fat_sdcard_unmount(STORAGE_SD_MOUNT_POINT, s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD card unmount failed (%s)", esp_err_to_name(err));
        return err;
    }

    s_card = NULL;
    ESP_LOGI(TAG, "SD card unmounted");
    return ESP_OK;
}

esp_err_t storage_sdcard_find_first_mp3(char *out_path, size_t out_path_len)
{
    if ((out_path == NULL) || (out_path_len == 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!storage_sdcard_is_mounted()) {
        esp_err_t err = storage_sdcard_mount();
        if (err != ESP_OK) {
            return err;
        }
    }

    static const char *search_dirs[] = {
        STORAGE_SD_MOUNT_POINT "/mp3",
        STORAGE_SD_MOUNT_POINT,
    };

    for (size_t i = 0; i < (sizeof(search_dirs) / sizeof(search_dirs[0])); i++) {
        esp_err_t err = storage_find_first_mp3_in_dir(search_dirs[i], out_path, out_path_len);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Found MP3 file: %s", out_path);
            return ESP_OK;
        }
    }

    return ESP_ERR_NOT_FOUND;
}
