#include "gif_storage.h"
#include <esp_log.h>
#include <esp_spiffs.h>
#include <esp_heap_caps.h>
#include <sys/stat.h>
#include <dirent.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>

static const char* TAG = "GifStorage";
static bool s_initialized = false;
static gif_storage_progress_callback_t s_progress_callback = NULL;
static void* s_progress_user_data = NULL;

#define STORAGE_BASE_PATH "/storage"
#define STORAGE_PARTITION_LABEL "storage"

esp_err_t gif_storage_init(void) {
    if (s_initialized) {
        ESP_LOGW(TAG, "GIF storage already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing GIF storage...");

    esp_vfs_spiffs_conf_t conf = {
        .base_path = STORAGE_BASE_PATH,
        .partition_label = STORAGE_PARTITION_LABEL,
        .max_files = 10,
        .format_if_mount_failed = false  // Don't auto-format, require explicit formatting
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGW(TAG, "Failed to mount filesystem, formatting...");
            // Format the partition and try again
            ret = esp_spiffs_format(STORAGE_PARTITION_LABEL);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to format SPIFFS partition: %s", esp_err_to_name(ret));
                return ret;
            }
            ESP_LOGI(TAG, "SPIFFS partition formatted successfully, retrying mount");
            ret = esp_vfs_spiffs_register(&conf);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to mount after format: %s", esp_err_to_name(ret));
                return ret;
            }
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition '%s'", STORAGE_PARTITION_LABEL);
            return ret;
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
            return ret;
        }
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(STORAGE_PARTITION_LABEL, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
        esp_vfs_spiffs_unregister(STORAGE_PARTITION_LABEL);
        return ret;
    }

    ESP_LOGI(TAG, "GIF storage initialized successfully");
    ESP_LOGI(TAG, "Partition size: total: %d bytes, used: %d bytes", total, used);
    
    s_initialized = true;
    return ESP_OK;
}

esp_err_t gif_storage_deinit(void) {
    if (!s_initialized) {
        return ESP_OK;
    }

    esp_err_t ret = esp_vfs_spiffs_unregister(STORAGE_PARTITION_LABEL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to unregister SPIFFS (%s)", esp_err_to_name(ret));
        return ret;
    }

    s_initialized = false;
    ESP_LOGI(TAG, "GIF storage deinitialized");
    return ESP_OK;
}

esp_err_t gif_storage_read(const char* filename, uint8_t** out_data, size_t* out_size) {
    if (!s_initialized) {
        ESP_LOGE(TAG, "GIF storage not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!filename || !out_data || !out_size) {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }

    // Build full path
    char filepath[256];
    snprintf(filepath, sizeof(filepath), "%s/%s", STORAGE_BASE_PATH, filename);

    ESP_LOGI(TAG, "Reading GIF file: %s", filepath);

    // Get file size
    struct stat st;
    if (stat(filepath, &st) != 0) {
        ESP_LOGE(TAG, "File not found: %s", filepath);
        return ESP_ERR_NOT_FOUND;
    }

    size_t file_size = st.st_size;
    if (file_size == 0) {
        ESP_LOGE(TAG, "File is empty: %s", filepath);
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(TAG, "File size: %d bytes", file_size);

    // Allocate buffer in PSRAM if available, otherwise internal RAM
    uint8_t* buffer = (uint8_t*)heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer) {
        ESP_LOGW(TAG, "Failed to allocate in PSRAM, trying internal RAM");
        buffer = (uint8_t*)heap_caps_malloc(file_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!buffer) {
            ESP_LOGE(TAG, "Failed to allocate %d bytes for GIF", file_size);
            return ESP_ERR_NO_MEM;
        }
    }

    // Open and read file
    FILE* f = fopen(filepath, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file: %s", filepath);
        heap_caps_free(buffer);
        return ESP_ERR_NOT_FOUND;
    }

    size_t bytes_read = fread(buffer, 1, file_size, f);
    fclose(f);

    if (bytes_read != file_size) {
        ESP_LOGE(TAG, "Failed to read complete file: read %d of %d bytes", bytes_read, file_size);
        heap_caps_free(buffer);
        return ESP_FAIL;
    }

    // Verify GIF header
    if (bytes_read < 6 || (memcmp(buffer, "GIF87a", 6) != 0 && memcmp(buffer, "GIF89a", 6) != 0)) {
        ESP_LOGE(TAG, "Invalid GIF file format");
        heap_caps_free(buffer);
        return ESP_ERR_INVALID_ARG;
    }

    *out_data = buffer;
    *out_size = file_size;

    ESP_LOGI(TAG, "Successfully read GIF file: %s (%d bytes)", filename, file_size);
    return ESP_OK;
}

void gif_storage_set_progress_callback(gif_storage_progress_callback_t callback, void* user_data) {
    s_progress_callback = callback;
    s_progress_user_data = user_data;
}

esp_err_t gif_storage_write(const char* filename, const uint8_t* data, size_t size) {
    if (!s_initialized) {
        ESP_LOGE(TAG, "GIF storage not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!filename || !data || size == 0) {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }

    // Build full path
    char filepath[256];
    snprintf(filepath, sizeof(filepath), "%s/%s", STORAGE_BASE_PATH, filename);

    ESP_LOGI(TAG, "Writing file: %s (%zu bytes)", filepath, size);

    // Check available space before writing
    size_t total, used;
    esp_err_t space_ret = esp_spiffs_info(STORAGE_PARTITION_LABEL, &total, &used);
    if (space_ret == ESP_OK) {
        ESP_LOGI(TAG, "Before write - Total: %zu, Used: %zu, Free: %zu", total, used, total - used);
    }

    // Open file for writing
    FILE* f = fopen(filepath, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to create file: %s (errno: %d)", filepath, errno);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "File opened successfully, attempting to write %zu bytes", size);
    if (s_progress_callback) {
        s_progress_callback(0, size, s_progress_user_data);
    }

    // Write data in chunks to debug
    size_t written = 0;
    const size_t chunk_size = 8192;  // Write in 8KB chunks
    const uint8_t* ptr = data;

    while (written < size) {
        size_t to_write = (size - written > chunk_size) ? chunk_size : (size - written);
        size_t chunk_written = fwrite(ptr, 1, to_write, f);

        if (chunk_written == 0) {
            ESP_LOGE(TAG, "fwrite returned 0 at offset %zu, ferror=%d, feof=%d", written, ferror(f), feof(f));
            break;
        }

        written += chunk_written;
        ptr += chunk_written;

        if (s_progress_callback) {
            s_progress_callback(written, size, s_progress_user_data);
        }

        if (written % (64 * 1024) == 0) {  // Log every 64KB
            ESP_LOGI(TAG, "Written %zu / %zu bytes", written, size);
        }
    }

    fclose(f);

    if (written != size) {
        ESP_LOGE(TAG, "Failed to write complete file: %s (wrote %zu of %zu bytes)", filepath, written, size);
        // Try to remove incomplete file
        unlink(filepath);
        if (s_progress_callback) {
            s_progress_callback(written, size, s_progress_user_data);
        }
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Successfully wrote file: %s (%zu bytes)", filename, size);
    if (s_progress_callback) {
        s_progress_callback(size, size, s_progress_user_data);
    }
    return ESP_OK;
}

bool gif_storage_exists(const char* filename) {
    if (!s_initialized || !filename) {
        return false;
    }

    char filepath[256];
    snprintf(filepath, sizeof(filepath), "%s/%s", STORAGE_BASE_PATH, filename);

    struct stat st;
    return (stat(filepath, &st) == 0);
}

esp_err_t gif_storage_list(gif_storage_list_callback_t callback, void* user_data) {
    if (!s_initialized) {
        ESP_LOGE(TAG, "GIF storage not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!callback) {
        ESP_LOGE(TAG, "Invalid callback");
        return ESP_ERR_INVALID_ARG;
    }

    DIR* dir = opendir(STORAGE_BASE_PATH);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open storage directory");
        return ESP_FAIL;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        // Skip directories and hidden files
        if (entry->d_type == DT_DIR || entry->d_name[0] == '.') {
            continue;
        }

        // Get file size (use larger buffer to avoid truncation warning)
        char filepath[512];
        snprintf(filepath, sizeof(filepath), "%s/%s", STORAGE_BASE_PATH, entry->d_name);
        struct stat st;
        if (stat(filepath, &st) == 0) {
            callback(entry->d_name, st.st_size, user_data);
        }
    }

    closedir(dir);
    return ESP_OK;
}

esp_err_t gif_storage_info(size_t* total_bytes, size_t* used_bytes) {
    if (!s_initialized) {
        ESP_LOGE(TAG, "GIF storage not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    return esp_spiffs_info(STORAGE_PARTITION_LABEL, total_bytes, used_bytes);
}

esp_err_t gif_storage_delete(const char* filename) {
    if (!s_initialized) {
        ESP_LOGE(TAG, "GIF storage not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!filename) {
        ESP_LOGE(TAG, "Invalid filename");
        return ESP_ERR_INVALID_ARG;
    }

    char filepath[256];
    snprintf(filepath, sizeof(filepath), "%s/%s", STORAGE_BASE_PATH, filename);

    if (unlink(filepath) != 0) {
        ESP_LOGE(TAG, "Failed to delete file: %s", filename);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Deleted file: %s", filename);
    return ESP_OK;
}

esp_err_t gif_storage_get_info(size_t* total_bytes, size_t* used_bytes) {
    return gif_storage_info(total_bytes, used_bytes);
}
