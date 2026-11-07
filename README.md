# 初代 ATOM Echo 小智客户端

适用于 ESP32-PICO-D4、4 MB Flash 的 M5Stack ATOM Echo。板子通过 Wi-Fi 直接连接小智官方云，首次配网后可用 USB 电源独立运行。按住正面按键讲话，松开提交。当前版本支持按键对话。

首次开机会通过 115200 波特率串口打印以 Xiaozhi-Echo 开头的热点名和随机密码。连接该热点，在浏览器输入 http://192.168.4.1 ，填写可联网的 2.4 GHz Wi-Fi。联网后刷新页面查看激活码，登录 https://xiaozhi.me 控制台添加设备。设备每十秒检查一次绑定状态。重新配网时按住正面按钮并重新上电。

板载状态灯使用低亮度常亮。橙色表示正在连接，黄色表示配网或等待绑定，暗绿表示可以讲话，按住录音时变红，松开等待回复时变紫，播放回复时为青蓝色。

麦克风使用 GPIO33 时钟、GPIO23 数据的单声道 PDM 输入。扬声器使用 GPIO19 位时钟、GPIO33 字时钟和 GPIO22 数据的 I²S 输出。收音和播放交替占用 I²S0。启动时短促的提示音用于检查扬声器，串口输出麦克风峰值和 Opus 自检结果。

## 构建和刷写

安装 PlatformIO 后，用 pio device list 查看设备，将 PORT 替换为实际端口。

```sh
pio run
pio run -t upload --upload-port PORT
pio device monitor -p PORT -b 115200
```

项目包含面向 Xtensa ESP32 编译的 Xiph.Org Opus 1.3.1 静态库。其许可证位于 vendor/COPYING。重建该库可使用 PlatformIO 的 toolchain-xtensa-esp32，在官方源码目录执行以下命令。opus-1.3.1.tar.gz 的 SHA-256 为 65b58e1e25b2a114157014736a3d9dfeaad8d41be1c8179866f144a2fb44ff9d。

```sh
export PATH="$HOME/.platformio/packages/toolchain-xtensa-esp32/bin:$PATH"
CFLAGS='-Os -mlongcalls -ffunction-sections -fdata-sections -DFIXED_POINT=1 -DVAR_ARRAYS -DOPUS_BUILD' ./configure --host=xtensa-esp32-elf --enable-fixed-point --disable-float-api --disable-rtcd --disable-asm --disable-extra-programs --disable-doc --disable-shared --enable-static --disable-stack-protector ac_cv_func_malloc_0_nonnull=yes ac_cv_func_realloc_0_nonnull=yes
make -j4 libopus.la
```

将 .libs/libopus.a 复制到本项目的 vendor 目录。Wi-Fi 密码和设备 UUID 保存在设备 NVS 中。云端证书通过 DigiCert 根证书校验，首次连接需要网络可以访问 NTP 与 api.tenclass.net。
