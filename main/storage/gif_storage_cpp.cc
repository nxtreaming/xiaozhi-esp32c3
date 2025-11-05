/**
 * @file gif_storage_cpp.cc
 * @brief C++ wrapper functions for gif_storage
 * 
 * This file contains C++ specific functions that use STL containers.
 * These functions are separated from the main C implementation to avoid
 * compilation issues when the C file is compiled with a C compiler.
 */

#include "gif_storage.h"
#include <esp_log.h>
#include <dirent.h>
#include <vector>
#include <string>

static const char* TAG = "GifStorageCpp";

#define STORAGE_BASE_PATH "/storage"

extern "C" {
    // Forward declaration of the initialization check function
    extern bool gif_storage_is_initialized(void);
}

// Check if storage is initialized (we need to access the static variable from C file)
static bool is_storage_initialized() {
    // We'll call a simple function to check if we can access the storage
    // If gif_storage_info succeeds, storage is initialized
    size_t total, used;
    return (gif_storage_info(&total, &used) == ESP_OK);
}

esp_err_t gif_storage_list_files(std::vector<std::string>& files) {
    if (!is_storage_initialized()) {
        ESP_LOGE(TAG, "GIF storage not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    files.clear();

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
        files.push_back(std::string(entry->d_name));
    }

    closedir(dir);
    ESP_LOGI(TAG, "Listed %zu files", files.size());
    return ESP_OK;
}
