# ESP-VoCat 喵伴

## 简介

<div align="center">
    <a href="https://oshwhub.com/esp-college/echoear"><b> 立创开源平台 </b></a>
</div>

ESP-VoCat 喵伴是一款智能 AI 开发套件，搭载 ESP32-S3-WROOM-1 模组，1.85 寸 QSPI 圆形触摸屏，双麦阵列，支持离线语音唤醒与声源定位算法。硬件详情等可查看[立创开源项目](https://oshwhub.com/esp-college/echoear)。

## **⚠️ 重要**IDF版本

**建议使用IDF6.0.2，本人在5.5.x的版本上编译遇见了各种各样的问题，IDF大版本更新还是有许多差别的，所以建议使用这个版本，暂时还没去研究为什么会出现那些问题，所以这里版本要求6.0.2**

## 配置、编译命令

**配置编译目标为 ESP32S3**

```bash
idf.py set-target esp32s3
```

**打开 menuconfig 并配置，可见基本配置**

```bash
idf.py menuconfig
```

### 基本配置
- `Xiaozhi Assistant` → `Board Type` →  `Espressif ESP-VoCat`

### UI风格选择

ESP-VoCat 支持多种不同的 UI 显示风格，通过 menuconfig 配置选择：

- `Xiaozhi Assistant` → `Select display style` → 选择显示风格

#### 可选风格

##### 表情动画风格 (Emote animation style) - 推荐
- **配置选项**: `USE_EMOTE_MESSAGE_STYLE` (默认)
- **特点**: 使用自定义的 `EmoteDisplay` 表情显示系统
- **功能**: 支持丰富的表情动画、眼睛动画、状态图标显示
- **适用**: 智能助手场景，提供更生动的人机交互体验
- **类**: `emote::EmoteDisplay`

**⚠️ 重要**: 选择此风格后默认，无需更改：
1. `Xiaozhi Assistant` → `Flash Assets` →  `Flash Emote Assets`
> 定义宏 `FLASH_EXPRESSION_ASSETS` 这样CMake文件才会编译链接出 `expression_assets.bin` 表情才会生效

##### 默认消息风格 (Enable default message style)
- **配置选项**: `USE_DEFAULT_MESSAGE_STYLE` 
- **特点**: 使用标准的消息显示界面
- **功能**: 传统的文本和图标显示界面
- **适用**: 标准的对话场景
- **类**: `SpiLcdDisplay`

##### 微信消息风格 (Enable WeChat Message Style)
- **配置选项**: `USE_WECHAT_MESSAGE_STYLE`
- **特点**: 仿微信聊天界面风格
- **功能**: 类似微信的消息气泡显示
- **适用**: 喜欢微信风格的用户
- **类**: `SpiLcdDisplay`

> **说明**: ESP-VoCat 喵伴使用16MB Flash，需要使用专门的分区表配置来合理分配存储空间给应用程序、OTA更新、资源文件等。且部分分区在CMake文件中，分区名字为 `assets` 如要修改，对应好名字或者修改 CMake文件，烧录表见到 `build\flash_args`

按个人喜好配置完成后，按 `S` 保存，按 `Q` 退出。

**编译**

```bash
idf.py build
```

**烧录**

将 ESP-VoCat 喵伴连接至电脑，**注意打开电源**，并运行：

```bash
idf.py flash
```
