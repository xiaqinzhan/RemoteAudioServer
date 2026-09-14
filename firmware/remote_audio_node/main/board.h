/**
 * board.h —— 板级引脚定义入口
 *
 * 当前目标板：立创实战派 ESP32-S3 (LCKFB SZPI)
 * 切换板型：修改下方 #include 即可
 */
#pragma once

/* ====== 选择目标板 ====== */
#define BOARD_TYPE_LCKFB_S3   1
/* #define BOARD_TYPE_DEVKITC   2 */  /* 原 ESP32-S3-DevKitC-1 + INMP441 */

#if defined(BOARD_TYPE_LCKFB_S3)
    #include "board_lckfb_s3.h"
#elif defined(BOARD_TYPE_DEVKITC)
    /* 原 ESP32-S3-DevKitC-1 + INMP441 的引脚文件 board_devkitc.h 已不在本工程里，
     * 如果确实要回退到那套硬件，请先把这个头文件补回来再打开上面的宏。 */
    #error "board_devkitc.h 不在本工程中，请先补回该板级头文件再切换到 DevKitC"
#else
    #error "未定义 BOARD_TYPE，请在 board.h 中选择目标板"
#endif
