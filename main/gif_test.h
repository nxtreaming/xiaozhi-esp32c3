#ifndef GIF_TEST_H
#define GIF_TEST_H

#include <cstdint>
#include <cstddef>

// 示例：一个简单的测试GIF数据
extern const uint8_t test_gif_data[];
extern const size_t test_gif_size;

// GIF测试函数
void test_gif_display();
void test_gif_from_url(const char* url);
void stop_gif_display();

#endif // GIF_TEST_H
