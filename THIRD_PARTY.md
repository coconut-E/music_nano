# 第三方组件与许可证

本项目基于 [MIT License](LICENSE) 开源。以下第三方组件通过 ESP-IDF Component Manager
（`main/idf_component.yml`）自动拉取（`managed_components/` 不纳入版本控制），或作为本地覆盖组件
放在 `third_party/`。它们各自遵循其原始许可证，使用/分发时请遵守相应条款。

| 组件 | 版本 | 许可证 | 来源 |
| --- | --- | --- | --- |
| [ESP-IDF](https://github.com/espressif/esp-idf) | ≥ 5.5.5 | Apache-2.0 | Espressif |
| [LVGL](https://github.com/lvgl/lvgl) | 8.3.11 | MIT | LVGL |
| [esp_lcd_jd9853](https://components.espressif.com/components/mydazy/esp_lcd_jd9853) | 2.0.0 | 以上游仓库为准（Apache-2.0） | mydazy |
| [micro-mp3](https://components.espressif.com/components/esphome/micro-mp3) | 0.4.0 | 以上游仓库为准 | esphome |
| [micro-flac](https://components.espressif.com/components/esphome/micro-flac) | 0.2.0 | 以上游仓库为准 | esphome |
| [JPEGDEC](https://github.com/bitbank2/JPEGDEC) | 1.6.2 | Apache-2.0 | BitBank Software（本地覆盖于 `third_party/jpegdec`） |

> 注：`third_party/jpegdec` 为本地覆盖版本（修复了三分量 1x2 采样封面的支持），其许可证见
> [third_party/jpegdec/LICENSE](third_party/jpegdec/LICENSE)。其余组件在首次构建时由组件管理器下载，
> 对应许可证文件位于各自组件目录内。
