idf_component_register(SRCS "main.c"
                            "app/console.c"
                            "app/settings.c"
                            "app/played_bits.c"
                            "app/text_encoding.c"
                            "app/song_hash.c"
                            "app/likes.c"
                            "app/lyrics.c"
                            "app/app_mode.c"
                            "app/ota.c"
                            "app/update.c"
                            "app/wifi_mgr.c"
                            "app/wifi_cred.c"
                            "audio/audio_task.cpp"
                            "audio/decoder_mp3.cpp"
                            "audio/decoder_flac.cpp"
                            "audio/decoder_wav.cpp"
                            "audio/pcm_pipeline.cpp"
                            "audio/impl/pcm_convert.cpp"
                            "audio/resampler_fxp.cpp"
                            "audio/cover.cpp"
                            "bt/bt_a2dp.c"
                            "storage/sys_monitor.c"
                            "storage/media_scan.c"
                            "power/power_mgr.c"
                            "ui/ui_core.c"
                            "ui/ui_loop.c"
                            "ui/ui_player.c"
                            "ui/ui_lyrics.c"
                            "ui/ui_shell.c"
                            "ui/ui_novel.c"
                            "ui/ui_menu.c"
                            "ui/ui_settings.c"
                            "ui/ui_settings_wifi.c"
                            "ui/mode_anim.c"
                            "ui/menu/menu_anim.c"
                            "ui/menu/menu_browser.c"
                            "ui/menu/menu_bt.c"
                            "ui/menu/menu_like.c"
                            "ui/menu/menu_mode.c"
                            "drivers/drv_display.c"
                            "drivers/touch.c"
                            "resources/icon_file.c"
                            "resources/icon_heart.c"
                            "resources/icon_novel.c"
                            "resources/icon_menu_music.c"
                            "resources/icon_menu_novel.c"
                            "resources/icon_menu_settings.c"
                            "resources/lv_font_global_16.c"
                            "resources/lv_font_heart_44.c"
                            "resources/font_part.c"
                    PRIV_REQUIRES bt nvs_flash fatfs esp_lcd_jd9853 esp_lcd driver micro-mp3 micro-flac esp_adc esp_psram jpegdec esp_partition app_update
                                  esp_wifi esp_event esp_netif lwip esp_http_client mbedtls
                    INCLUDE_DIRS "." "app" "audio" "bt" "storage" "ui" "ui/menu" "drivers" "resources" "power")

# 更新服务器的根证书 (PEM). 里面是"本地自签 CA + ISRG Root X1 (+X2)"拼接,
# 由 tools/ota_cert.ps1 生成; 换服务器只改这个文件, 不用改代码.
# 用 TXTFILES 是为了自动补末尾的 NUL —— mbedtls 按字符串取 CN, 需要它.
target_add_binary_data(${COMPONENT_LIB} "certs/ota_ca.pem" TEXT)

# 测试开关 (build.ps1 -TestCrash): 编译一版"一启动就 abort"的固件,
# 用来验证 OTA 回退 (镜像能过校验, 但跑不起来).
# 由 build.ps1 每次都显式传 0/1, 避免 CMake 缓存把上一次的值带过来。
if(TEST_CRASH_ON_BOOT)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE TEST_CRASH_ON_BOOT=1)
    message(STATUS "main: 测试固件 TEST_CRASH_ON_BOOT=1 (启动即崩)")
endif()
