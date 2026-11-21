# AVRCP 1.6 高清封面试验（ESP32-S31 / HyperOS 2）

## 背景

原生 AVRCP Cover Art 已在红米 K50（Android 14 / HyperOS 2.0.10.0）+ QQ 音乐成功显示部分封面。但当前 FakePod 只调用 `esp_avrc_ct_cover_art_get_linked_thumbnail`，这是低分辨率缩略图；部分歌曲偶发缺图。稳定基线：`feat/phone-avrcp-cover-art` 修改前的 commit `7c56126b9dc9e1d4f8ad98e67c87c98f0ed42837`。

## 本次实现

- 遵循 ESP-IDF release/v6.1：封面句柄元数据（0x80） → `GetImageProperties`（诊断/可选） → `GetImage(handle, nullptr, 0)`（请求原始/原生图）→ `GetLinkedThumbnail`（失败后备）。
- 对原生高清图和缩略图沿用原来的 PSRAM 暂存与 JPEG/PNG 校验，下载完成后仍交给后台 CoverSurface 解码，不在 Bluedroid 回调中解码。
- 原生图片缓存由 192 KiB 增至 **512 KiB**（外部 PSRAM）。超过限制的 BIP 图不写越界，等该响应结束后回退缩略图。不改变 Android Remote 的 BLE 上传上限。
- 图片操作同一时刻只有一个在执行。切歌后把旧下载标为 stale 并等待结束；旧响应不会污染新歌缓存。
- 属性查询即使返回失败也尝试 GetImage；原生 GetImage 失败后尝试缩略图；缩略图失败最多重试两次（有退避），然后停止等待下一次曲目变化。
- OBEX 断开时延迟重连，限制连续连接失败次数；下载 10 秒无新数据或总耗时 45 秒时发出断开重置，待重连继续后备流程。保持既有 A2DP 音频路径不变。
- 串口日志增加 `AVRCP_COVER request=properties/native/thumbnail`、`AVRCP_COVER <mode> completed ok=... bytes=...` 和超时、重连情况。CoverSurface 已有原图尺寸、解码耗时记录。

## 实机验证（需用户在本地完成）

编译之前检查所用 ESP-IDF 是否为支持 ESP32-S31 的 release/v6.1，且 `CONFIG_BT_AVRCP_CT_COVER_ART_ENABLED=y`。固件编译/烧录在原本项目环境执行；请在真机测试后才考虑合并到主分支。

1. 红米 K50 保持开发者选项 **AVRCP 1.6**；连接 `FakePod Audio`。Android Remote 的 BLE 封面同步先关闭，排除重复来源。
2. 先用原来有封面的歌 A，再切到以前会模糊的歌 B；观察串口 `AVRCP_COVER request=properties` → `request=native` → `native completed ok=1` → `surface requested` / `当前曲封面完成`。
3. 测试本身无封面、下载超时、快速连续切歌、暂停恢复、从 A2DP 接收切到其他模式。
4. 如果 `native ... ok=0`，确认随后能看到 `request=thumbnail` 并最终显示。
5. 如果原图下载成功但屏幕仍不显示，检查 `封面预处理`/CoverSurface 的尺寸和解码失败日志。过大像素尺寸或格式不支持可能发生在下载之后，需再补解码后备。
6. 如果音频出现卡顿，请报告 A2DP 音频统计、下载图像字节数和 OBEX 耗时；原生图会占用更多 Classic BT 带宽。

### 预期日志关键词

```text
AVRCP_PEER
AVRCP_COVER peer_support=1
AVRCP_COVER OBEX=CONNECTED
AVRCP_COVER image handle metadata
AVRCP_COVER request=properties
AVRCP_COVER properties completed
AVRCP_COVER request=native
AVRCP_COVER native completed ok=1 bytes=...
AVRCP_COVER surface requested
当前曲封面完成
```

### 注意

“原生图”不等于严格的 460×460。使用空 Image Descriptor 是规范允许的原生图请求；手机可能返回不同编码、尺寸和字节数。对明确要求 460×460 的图片描述符，应先通过 `GetImageProperties` 解析出手机支持的表示格式后再协商，不应猜测尺寸。此版本优先确保兼容性和可靠回退。

代码尚未在使用者的 ESP-IDF 6.1 环境完整构建，也尚未经红米 K50 真机验证；日志和分支提交不是实机成功的证明。

## V1.1 修复 ESP_ERR_INVALID_ARG（2026-11-21）

用户真机日志已验证此前版本 `GetImageProperties` 成功，但 `GetImage(handle, nullptr, 0)` **每首歌都返回 `ESP_ERR_INVALID_ARG`**，因此仍只显示 200×200 Thumbnail。原因不是手机不支持高清，而是 ESP-IDF 6.1 的 `esp_avrc_ct_cover_art_get_image()` 强制要求非空、非零长度 Image Descriptor。

现在增加 `src/system/bt_avrc_cover_descriptor.h`，读取 Properties 返回的 BIP XML：如果手机声明可变尺寸 JPEG，优先请求 460×460；否则请求 JPEG 原生分辨率且 Properties 声明文件大小不超过 512 KiB；没有安全的候选或图片下载失败时自动降级 Thumbnail。有效的 Descriptor 形如：

```xml
<image-descriptor version="1.0"><image encoding="JPEG" pixel="460*460"/></image-descriptor>
```

下载前可看到如下诊断（尺寸取决于手机实际提供的属性）：

```text
AVRCP_COVER properties XML: ...
AVRCP_COVER image descriptor jpeg=460x460 variant=1 len=...
AVRCP_COVER request=native err=ESP_OK max_bytes=524288
AVRCP_COVER native completed ok=1 bytes=...
JPEG_HW ... 460x460 ...
```

真实输出若有 `request=native err=ESP_OK` 但 `native completed ok=0`，说明请求已经发送到手机但被拒绝或传输失败；应继续观察缩略图后备。

独立 C++ 测试：`g++ -std=c++17 -Wall -Wextra -Werror -Isrc tests/avrcp_cover_descriptor_smoke.cpp -o cover_smoke && ./cover_smoke`。

**额外性能观察**：真机日志在 A2DP 开始播放的早期出现 `A2DP_RX drop=3200`，下一次为 3328、后续稳定 3328。这不是该版本高清图的测试结果，因为高清请求尚未实际发出。测试新版时应比较 `drop` 增量与 `native completed` 时间的对应关系，避免以累计 drop 误判持续音频中断。
