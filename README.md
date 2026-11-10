# zwrt-devui

**简体中文** | [English](README.en.md)

为 5G 随身 Wi-Fi 前面板小屏重新实现的开源界面。程序只负责渲染，界面全部是 HTML/CSS，改页面不用重新编译。

![界面截图](docs/images/screens.png)

## 特点

- **界面即数据**：页面放在 `/data/plugins/zwrt-devui/ui`，改完 HTML 约 1 秒内自动生效。
- **单个静态二进制**：aarch64 静态 ELF，无运行时依赖，基于标准 DRM/KMS 和 evdev。
- **实时数据**：从配套后端 [zwrt-datad](https://github.com/33333s/zwrt-datad) 读取状态，设备操作也经它下发。
- **自带完整界面**：首页、信号、功能、图表、系统设置，控制中心、短信、Wi-Fi、锁频、锁屏和电源菜单，四套主题。
- **壁纸与液态玻璃**：在插件里换壁纸，也可以用一段视频做动态壁纸（最长 20 秒），按钮是半透明的液态玻璃，透明度可调。
- **外部画面接口**：其他本机程序可以借用屏幕显示内容，见 [DEVUI-IPC](docs/DEVUI-IPC.md)。

## 支持设备

| 设备 | 型号 | 说明 |
| --- | --- | --- |
| U60 Pro | MU5250 | 默认界面 |
| TopFlow | MU5252 | 多基带信号卡片、链路聚合、风扇与液冷控制 |

机型自动识别，不同机型的页面按文件名区分。

## 安装

需要先装好后端 [zwrt-datad](https://github.com/33333s/zwrt-datad)。从 [Releases](https://github.com/33333s/zwrt-devui/releases) 下载 `zwrt-devui-aarch64` 和 `ui.tar.gz`，连同仓库里的 [`scripts/start.sh`](scripts/start.sh) 放到设备的 `/data/plugins/zwrt-devui/`，然后：

```sh
cd /data/plugins/zwrt-devui
mv zwrt-devui-aarch64 zwrt-devui && chmod 755 zwrt-devui
mkdir -p ui && tar -xzf ui.tar.gz -C ui
sh start.sh
```

`start.sh` 会停掉原厂屏幕程序再启动 zwrt-devui。需要开机自启时运行 [`scripts/install-autostart.sh`](scripts/install-autostart.sh)。

## 原生 OpenWrt（`u60pro-openwrt` 分支）

这个分支多了一个数据后端 `src/data_ubus.c`，给不带原厂用户态、也不跑 zwrt-datad 的系统用（U60 Pro 的原生 OpenWrt）：

```sh
BACKEND=ubus bash scripts/build.sh
```

- 状态直接从 ubus 和 UCI 取：系统自己有的走标准对象（`system`、`network.interface`、`network.device`、`network.wireless`、`iwinfo`、UCI `dhcp`），
  电池、背光这些走设备自己的 `u60pro.hw`，蜂窝走 `u60pro.cell`（没有时显示上行接口）。
- 操作换成对应的标准调用（`network.wireless up/down`、`system reboot` 等）；亮度和自动息屏存在 UCI `u60pro.screen`，和网页共用。
- 原厂专用的开关（USB 模式、ADB、Wi-Fi 省电）在这个构建里不执行。
- 调用通过 `ubus`、`uci` 命令行，不链接 libubus；`DEVUI_UBUS`、`DEVUI_UCI` 可以换成别的程序（测试这么用）。
- 测试：`sh tests/run_host_tests.sh`；`BACKEND=ubus bash tests/ui/preview.sh 输出目录` 用本机（或上面两个变量指定的）ubus 渲染全部页面。

默认构建（`BACKEND=datad`）和以前完全一样。

## 自定义界面

每个顶层 `NN-名字.html` 是一页，`{{令牌}}` 会替换成实时数据，`href="act:xxx"` 触发操作。教程见 [docs/UI-GUIDE.md](docs/UI-GUIDE.md)。

## 构建

在 Linux 上：

```sh
bash scripts/_setup_toolchain.sh   # aarch64 musl 工具链（一次性）
bash scripts/_build_freetype.sh    # 静态 FreeType（一次性）
bash scripts/_build_litehtml.sh    # 静态 litehtml（一次性）
bash scripts/build.sh              # 产出 ./zwrt-devui 和 ./zwrt-devui.stripped
```

`bash tests/ui/preview.sh 输出目录` 可以在电脑上渲染全部页面。

## 交流

QQ 群 **625439164**

## 许可证

[MIT](LICENSE)。litehtml（BSD）、FreeType（FTL）、stb（public domain）按各自许可证使用。
