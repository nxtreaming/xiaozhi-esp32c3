#ifndef GIF_STORAGE_H
#define GIF_STORAGE_H

#include <esp_err.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the GIF storage system
 * 
 * This function mounts the SPIFFS partition for GIF storage.
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t gif_storage_init(void);

/**
 * @brief Deinitialize the GIF storage system
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t gif_storage_deinit(void);

/**
 * @brief Read a GIF file from storage
 * 
 * @param filename Name of the GIF file (e.g., "think.gif")
 * @param out_data Pointer to receive the allocated buffer containing GIF data
 * @param out_size Pointer to receive the size of the GIF data
 * 
 * @return ESP_OK on success, error code otherwise
 * 
 * @note The caller is responsible for freeing the allocated buffer using heap_caps_free()
 */
esp_err_t gif_storage_read(const char* filename, uint8_t** out_data, size_t* out_size);

/**
 * @brief Check if a GIF file exists in storage
 * 
 * @param filename Name of the GIF file
 * @return true if file exists, false otherwise
 */
bool gif_storage_exists(const char* filename);

/**
 * @brief List all GIF files in storage
 * 
 * @param callback Function to call for each file found
 * @param user_data User data to pass to callback
 * 
 * @return ESP_OK on success, error code otherwise
 */
typedef void (*gif_storage_list_callback_t)(const char* filename, size_t size, void* user_data);
esp_err_t gif_storage_list(gif_storage_list_callback_t callback, void* user_data);

/**
 * @brief Get storage information
 * 
 * @param total_bytes Pointer to receive total storage size
 * @param used_bytes Pointer to receive used storage size
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t gif_storage_info(size_t* total_bytes, size_t* used_bytes);

#ifdef __cplusplus
}
#endif

#endif // GIF_STORAGE_H

