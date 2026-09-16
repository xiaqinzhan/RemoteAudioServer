# main 组件构建脚本

# ------------------------------------------------------------------
# Opus 组件名自适应（在 idf_component_register 之前发现组件名）
# 不同发布渠道的托管组件目录名不同：
#   78/esp-opus            -> managed_components/78__esp-opus
#   espressif/esp-opus     -> managed_components/espressif__esp-opus
#   esphome/micro-opus     -> managed_components/esphome__micro-opus
# 组件管理器在 configure 阶段已下载到 managed_components/，
# 这里发现后将组件名加入 PRIV_REQUIRES，由构建系统自动处理链接。
# ------------------------------------------------------------------
set(_opus_component "")
file(GLOB _managed_opus_dirs LIST_DIRECTORIES true
     "${CMAKE_CURRENT_LIST_DIR}/../managed_components/*[Oo]pus*")
file(GLOB _local_opus_dirs LIST_DIRECTORIES true
     "${CMAKE_CURRENT_LIST_DIR}/../components/*[Oo]pus*")
foreach(_d ${_managed_opus_dirs} ${_local_opus_dirs})
    if(IS_DIRECTORY "${_d}")
        get_filename_component(_opus_comp_name "${_d}" NAME)
        set(_opus_component "${_opus_comp_name}")
        break()
    endif()
endforeach()

set(_opus_requires "")
if(_opus_component)
    set(_opus_requires "${_opus_component}")
    message(STATUS "Opus component found, adding to PRIV_REQUIRES: ${_opus_component}")
else()
    message(WARNING "Opus component not found in managed_components/ or components/. "
                    "Build will likely fail. Check idf_component.yml.")
endif()

idf_component_register(
    SRCS
        "main.c"
        "wifi_mgr.c"
        "wifi_cred.c"
        "wifi_prov.c"
        "ws_client.c"
        "audio_i2s.c"
        "vad.c"
        "opus_encoder.c"
        "ogg_muxer.c"
        "recorder.c"
        "sd_storage.c"
        "audio_stream.c"
        "diag.c"
        "device_config.c"
        "btn.c"
        "oled.c"
        # ES7210 驱动封装。文件名故意不叫 es7210.c：官方托管组件
        # espressif/es7210 的头文件就叫 es7210.h，同名会把它遮住。
        "es7210_drv.c"
    INCLUDE_DIRS
        "."
    # 配网页（wifi_prov.html）编译进固件，运行时由 esp_http_server 直接吐给手机。
    # 用 EMBED_TXTFILES 而不是 packed 版本：文本模式末尾会补 '\0'，
    # 正好用 HTTPD_RESP_USE_STRLEN 直接发，不必记长度。
    # 符号：_binary_wifi_prov_html_start / _end
    EMBED_TXTFILES
        "wifi_prov.html"
    PRIV_REQUIRES
        nvs_flash
        esp_wifi
        esp_http_server            # 配网页面 HTTP 服务（v2.5 新增）
        esp_event
        esp_netif
        esp_timer
        driver
        esp_driver_i2s
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_spi
        sdmmc
        fatfs
        vfs
        json
        esp_websocket_client
        esp-tls
        mbedtls                    # esp_crt_bundle.h（wss 证书校验用内置 CA bundle）
        espressif__es7210          # ES7210 ADC 寄存器配置（托管组件 espressif/es7210）
        lwip
        esp_hw_support
        ${_opus_requires}
)
