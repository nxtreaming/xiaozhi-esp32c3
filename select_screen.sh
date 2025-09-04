#!/bin/bash

CONFIG_FILE="main/boards/PANBOPO/config.h"

echo "请选择要启用的屏幕尺寸："
echo "1) EP1331S(1.28圆240*240)"
echo "2) EP1531S(1.54方240*240)"
echo "3) EP1531T(1.5寸圆360*360)"
echo "4) EP1831T(1.8寸圆360*360)"
echo "5) E2031CP(2.0寸圆240*320)"
echo "6) E2431BP(2.4寸方240*320)"
echo "7) E2831BP(2.8寸方240*320)"
echo "8) E3231BP(3.2寸方240*320)"
echo "9) E3531QP(3.5寸方320*480)"
echo "10) E4031BP(4.0寸方320*480)"
read -p "请输入选项 [1-10]: " choice

if [ ! -f "$CONFIG_FILE" ]; then
    echo "找不到配置文件: $CONFIG_FILE"
    exit 1
fi

# 替换宏定义的函数
set_macros() {
    sed -i 's/^#define EP1331S.*/#define EP1331S '"$1"'/' "$CONFIG_FILE"
    sed -i 's/^#define EP1531S.*/#define EP1531S '"$2"'/' "$CONFIG_FILE"
    sed -i 's/^#define EP1531T.*/#define EP1531T '"$3"'/' "$CONFIG_FILE"
    sed -i 's/^#define EP1831T.*/#define EP1831T '"$4"'/' "$CONFIG_FILE"
    sed -i 's/^#define E2031CP.*/#define E2031CP '"$5"'/' "$CONFIG_FILE"
    sed -i 's/^#define E2431BP.*/#define E2431BP '"$6"'/' "$CONFIG_FILE"
    sed -i 's/^#define E2831BP.*/#define E2831BP '"$7"'/' "$CONFIG_FILE"
    sed -i 's/^#define E3231BP.*/#define E3231BP '"$8"'/' "$CONFIG_FILE"
    sed -i 's/^#define E3531QP.*/#define E3531QP '"$9"'/' "$CONFIG_FILE"
    sed -i 's/^#define E4031BP.*/#define E4031BP '"$10"'/' "$CONFIG_FILE"
}

# 根据选择设置宏定义
case $choice in
    1)
        set_macros 1 0 0 0 0 0 0 0 0 0
        echo "✅ 已启用 EP1331S(1.28圆240*240)"
        ;;
    2)
        set_macros 0 1 0 0 0 0 0 0 0 0
        echo "✅ 已启用 EP1531S(1.54方240*240)"
        ;;
    3)
        set_macros 0 0 1 0 0 0 0 0 0 0
        echo "✅ EP1531T(1.5寸圆360*360)"
        ;;
    4)
        set_macros 0 0 0 1 0 0 0 0 0 0
        echo "✅ EP1831T(1.8寸圆360*360)"
        ;;
    5)
        set_macros 0 0 0 0 1 0 0 0 0 0
        echo "✅ E2031CP(2.0寸圆240*320)"
        ;;
    6)
        set_macros 0 0 0 0 0 1 0 0 0 0
        echo "✅ E2431BP(2.4寸方240*320)"
        ;;
    7)
        set_macros 0 0 0 0 0 0 1 0 0 0
        echo "✅ E2831BP(2.4寸方240*320)"
        ;;
    8)
        set_macros 0 0 0 0 0 0 0 1 0 0
        echo "✅ E3231BP(2.4寸方240*320)"
        ;;
    9)
        set_macros 0 0 0 0 0 0 0 0 1 0
        echo "✅ E3531QP(3.5寸方320*480)"
        ;;
    10)
        set_macros 0 0 0 0 0 0 0 0 0 1
        echo "✅ E4031BP(4.0寸方320*480)"
        ;;
        
    *)
        echo "无效选项，退出"
        exit 1
        ;;
esac

# 编译项目
echo "🛠️ 开始编译..."
if idf.py build; then
    # 检查关键文件是否存在
    if [ -f "build/xiaozhi.bin" ] && [ -f "build/bootloader/bootloader.bin" ]; then
        echo "✅ 编译成功"
        exit 0
    else
        echo "❌ 错误：未生成固件文件"
        exit 1
    fi
else
    echo "❌ 编译失败"
    exit 1
fi

# 烧录固件
# read -p "是否要烧录固件到设备？ [y/N]: " flash_choice
# if [[ "$flash_choice" =~ [yY] ]]; then
    echo "开始烧录固件..."
    if idf.py flash; then
        echo "✅ 烧录成功"
    else
        echo "❌ 烧录失败"
        exit 1
    fi
# else
    # echo "⏩ 跳过烧录步骤"
# fi
