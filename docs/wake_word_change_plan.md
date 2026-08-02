# ESP32 唤醒词更换与模型集成方案

## 当前产品策略

本仓库的角色和板型相互独立，但唤醒词属于产品角色：

- `ai-pet/`：角色“小智”，使用 ESP-SR 2.4.7 自带的
  `wn9_nihaoxiaozhi_tts`，短语为“你好，小智”。
- `anti-pet/`：角色“十神”，预留模型过滤词 `nihaoshishen`。在拿到真实的
  “你好，十神”模型前，默认关闭 WakeNet，通过屏幕“挑战小智”按钮主动发言。

角色参数分别位于 `ai-pet/main/app_role.c` 和 `anti-pet/main/app_role.c`；公共
WakeNet 加载、音频采集和状态机位于 `components/app_core/app_runtime.c`。因此
同一个角色可以换板，不需要修改角色行为；同一块板也可以编译任一角色项目。

WakeNet 是设备端声学模型，不是关键词字符串。修改 `wake_phrase` 只会改变日志
和界面，修改 `wake_model_filter` 只会选择已经存在的模型，二者都不能生成模型。

## “你好，小智”模型的来源

仓库没有本地训练流水线。“你好，小智”来自锁定版本 `espressif/esp-sr = 2.4.7`
已经发布的 WakeNet 模型包。它也是通过训练得到的模型，并非应用代码根据文字
即时生成。因此“你好，十神”可以重新训练，但必须先通过乐鑫的 TTS 社区申请或
商业定制流程取得模型文件。

官方入口：

- [ESP-SR WakeNet 文档](https://docs.espressif.com/projects/esp-sr/zh_CN/latest/esp32s3/wake_word_engine/README.html)
- [乐鑫唤醒词定制流程](https://docs.espressif.com/projects/esp-sr/zh_CN/latest/esp32s3/wake_word_engine/ESP_Wake_Words_Customization.html)
- [TTS 唤醒词社区申请（ESP-SR issue #88）](https://github.com/espressif/esp-sr/issues/88)

原型阶段可向 TTS Pipeline/社区申请提交：唤醒词“你好十神”、普通话、目标芯片
ESP32-S3、ESP-SR 版本、开源项目地址和用途。量产阶段应联系乐鑫完成真人语料、
噪声和误唤醒指标的定制与授权确认。

## 集成“你好，十神”模型

假设交付目录名为 `wn9_nihaoshishen_tts3`：

1. 将完整模型目录加入 ESP-SR 的 `model/wakenet_model/`。两个项目有各自的
   `managed_components/`，长期维护时应将模型制作成受版本控制的私有组件或
   自动补丁，避免依赖更新后丢失。
2. 在该 ESP-SR 组件的 `Kconfig.projbuild` 增加模型选项：

   ```kconfig
   config SR_WN_WN9_NIHAOSHISHEN_TTS3
       bool "你好十神 (wn9_nihaoshishen_tts3)"
       default n
   ```

   布尔默认值只能使用 `y` 或 `n`，不能使用 `True`、`False`、`0`、`1`。
3. 在 `anti-pet/sdkconfig.defaults` 增加：

   ```ini
   CONFIG_ENABLE_LOCAL_OFFLINE_WAKEUP=y
   CONFIG_SR_WN_WN9_NIHAOSHISHEN_TTS3=y
   ```
4. 确认 `anti-pet/main/app_role.c` 中的 `wake_model_filter` 能匹配模型目录名；当前
   已预留为 `nihaoshishen`。
5. 在 `anti-pet/` 内完整构建并烧录所有分区：

   ```bash
   cd anti-pet
   idf.py fullclean
   idf.py build
   idf.py -p /dev/ttyUSB0 flash monitor
   ```

只烧录 app 不够，因为 ESP-SR 会重新生成并烧录 `model` 分区。成功日志应包含：

```text
WakeNet ready: role=anti_pet phrase=你好，十神 model=<实际模型名>
```

如果模型缺失，设备应输出 `WakeNet unavailable`；反 AI 宠物的屏幕按钮仍可使用，
不会因为唤醒模型未交付而阻塞双设备测试。

## 切换到 ESP-SR 已有模型

如果角色接受“Hi ESP”“小爱同学”等现成词，可在该项目目录运行
`idf.py menuconfig`，只启用一个目标 WakeNet 模型，并同步修改该项目
`main/app_role.c` 的 `wake_phrase` 和 `wake_model_filter`。以
`build/srmodels/` 的实际模型清单和启动日志为准，不要根据显示名猜模型 ID。

## 验收标准

- 安静环境、背景音乐、多人说话和设备 TTS 回声期间分别测试。
- 目标词至少说 20 次并统计召回；旧词至少说 20 次验证不再触发。
- 未联网时目标词仍能触发设备端 WakeNet。
- 连续运行 30 分钟，无重复触发、内存持续下降、看门狗或重启。
- 保存 `WakeNet ready/detected`、复位原因、串口日志和现场录音。

原型建议目标：目标词召回率不低于 95%，旧词零触发，设备零重启。量产指标需用
更大规模、多说话人和真实噪声数据重新定义。
