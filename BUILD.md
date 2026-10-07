# 编译说明

## 环境准备

安装 **ESP-IDF ≥ 5.5.5**（低版本存在 SPI 总线死锁 bug），然后导出环境：

```powershell
# Windows PowerShell（将 <IDF_DIR> 替换为你的 esp-idf 路径）
. <IDF_DIR>\export.ps1
```

```bash
# Linux / macOS
. <IDF_DIR>/export.sh
```

确认 `idf.py` 可用：

```powershell
idf.py --version
```

## 直接编译

```powershell
idf.py set-target esp32
idf.py build
idf.py -p COMx flash monitor
```

## 使用 build.ps1（Windows，过滤噪声输出）

`build.ps1` 是对 `idf.py build` 的薄封装，只保留警告、错误与链接结果，屏蔽 CMake 配置和逐文件编译进度。

**一步完成**：若当前 shell 未导出 ESP-IDF 环境，脚本会自动探测系统里已安装的 ESP-IDF 并导入，无需手动执行 `export.ps1`（脚本内不硬编码任何本机路径）：

```powershell
.\build.ps1
```

自动探测顺序：当前环境变量 → `esp_idf.json` 安装记录（`IDF_TOOLS_PATH`、`%USERPROFILE%\.espressif`、VS Code 扩展目录、各盘符的 `Espressif` 目录）。若探测失败，可手动指定：

```powershell
.\build.ps1 -IdfPath <IDF_DIR> [-IdfToolsPath <TOOLS_DIR>]
```

已在当前 shell 导出过环境时，脚本会直接复用。清空 build 目录后重新编译：

```powershell
.\build.ps1 -Clean
```

## 内存占用分析

`mem-analyze.ps1` 读取构建产物的 ELF，统计常驻 RAM（DRAM/IRAM）与 Flash 段占用及最大的常驻符号：

```powershell
.\mem-analyze.ps1                 # 默认分析 build/ 下最新的 app ELF
.\mem-analyze.ps1 -Last 50        # 显示前 50 个最大符号
```

工具链路径默认取自 `$env:IDF_TOOLS_PATH`（导出环境后即已设置），可用 `-IdfTools` 覆盖。

## 注意事项

- 路径分隔符用 `/` 或 `\` 均可，CMake 会自动处理
- `sdkconfig.defaults` 中的 `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y` 已启用
- 首次完整构建步骤较多，增量编译仅编译变更的文件
