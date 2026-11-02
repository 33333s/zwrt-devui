# 自定义界面教程（写自己的 UI）

`zwrt-devui` 的设计是：**程序固定，界面是数据**。二进制本身不内置任何画面，它在运行时去 `/data/plugins/zwrt-devui/ui` 目录读取你写的 **HTML/CSS** 并渲染到屏幕。所以你想改界面，**完全不用重新编译**——改 HTML、推到设备、即时生效。

这份文档教你从零写一套自己的界面。

---

## 1. 它是怎么跑起来的

```text
后端 zwrt-datad ──▶ HTTP /state + SSE /events (127.0.0.1:9460) ──┐
                                                                  ├─▶ zwrt-devui ──▶ 屏幕
你写的 /data/plugins/zwrt-devui/ui/*.html + style.css ───────────┘
```

- 程序把 `/data/plugins/zwrt-devui/ui` 下每个 `*.html` 当作**一页**，左右滑动切换。
- 渲染前，程序会把 HTML 里的 `{{令牌}}` 替换成实时数据（电量、信号、WiFi 密码……）。
- 点击带 `href="act:xxx"` 的链接会触发**动作**（翻页、切主题、显示密码等），而不是跳转网页。
- HTML / CSS 仍然按文件热加载，所以 `adb push` 完不用重启，约 1 秒内自动生效；状态数据则来自本机 HTTP/SSE 缓存。

渲染引擎是 [litehtml](https://github.com/litehtml/litehtml)（一个 C++ 的 HTML/CSS 排版库）+ FreeType 字体。**没有浏览器、没有 JavaScript；状态只读本机 `127.0.0.1` 的 HTTP/SSE。**

---

## 2. 屏幕和目录

- 屏幕分辨率：**320 × 480**（竖屏）。所有页面都按这个尺寸设计。
- 目录结构：

```text
/data/plugins/zwrt-devui/ui/
├── 00-home.html      # 第 1 页（按文件名排序）：时钟、电量环、网络与速率；锁屏预览也显示这一页
├── 01-signal.html    # 第 2 页：RSRP/SINR 表盘 + 信号 / 载波
├── 02-functions.html # 第 2 页：更多功能入口
├── 05-charts.html    # 第 5 页：CPU / 内存 / 网速 图表
├── 06-system.html    # 第 6 页：系统信息 / 屏幕 / 开关（含锁屏开关）
├── subpages/         # 内置二级页面：wifi / sms / cell / lock / speedtest / glass，以及下拉的控制中心 control
├── functions/        # 用户自定义功能页，*.html 会自动出现在更多功能页
├── menu.html         # 电源键长按弹出的菜单（不算翻页）
├── lockscreen.html   # 锁屏 PIN 键盘（不算翻页，开启锁屏后覆盖显示）
└── style.css         # 所有页面共享的样式
```

- 文件名 `NN-名字.html` 的数字前缀决定**顺序**。想加一页，丢一个 `04-xxx.html` 进去即可，圆点指示会自动变成 4 个。
- `menu.html` 是特殊页：电源键长按时覆盖显示，里面放关机/重启/取消。
- 锁屏：启动、熄屏后、自动息屏为“关”时 60 秒无触摸，屏幕都会锁上，显示第 1 页（不响应点按）和底部“长按解锁”胶囊（原生绘制）。没设密码时按住 600 ms 解锁；设了密码时滑动或按住 450 ms 打开 `lockscreen.html` 密码盘。锁定时 `{{DOTS}}` 为空。
- `lockscreen.html` 是特殊页：开启锁屏后，超时/电源键锁屏时全屏显示 PIN 键盘（解锁正确才回到界面）；系统页锁屏开关从关→开时也用它设置新密码。
- `subpages/*.html` 是内置二级页面，通过 `act:sub:<文件名>` 打开，`act:backfunc` 返回顶层页；也可以从屏幕最左侧往右滑返回，页面跟着手指移动，滑过约三分之一屏宽或快速一划即返回，否则弹回。
- `functions/*.html` 是用户自定义二级页面，通过更多功能页自动扫描，示例见 `examples/custom-functions/`；示例不放进默认 `ui/`，避免安装后默认出现测试页面。
- 特殊页按全屏覆盖层处理，不继承普通页面的竖向滚动位置。
- 所有页面用 `<link rel="stylesheet" href="style.css">` 共享同一份样式。
- **按机型区分**：同一套界面同时支持 U60 Pro（MU5250）和 TopFlow（MU5252），机型取自 datad 的 `device.api_template`。文件名带机型的页面只在该机型显示，并替换同名的通用页：`03-link.mu5252.html` 只在 TopFlow 上出现，`00-home.mu5252.html` 在 TopFlow 上替换 `00-home.html`；`subpages/` 同理。`<body>` 的类里带 `dev-mu5250` 或 `dev-mu5252`，小差异用 CSS 隐藏即可，例如通用页里的 `class="row u60only"` 在 TopFlow 上不显示，`class="row tfonly"` 只在 TopFlow 上显示。调试时可用环境变量 `DEVUI_DEVICE=mu5252` 强制机型。
- 旧版若还残留 `/data/ui`，新版安装脚本会自动把它迁移到这里。

---

## 3. 一个最小页面

```html
<!DOCTYPE html>
<html lang="zh-CN">
<head><meta charset="UTF-8"><link rel="stylesheet" href="style.css"></head>
<body class="{{THEME}}">
  {{STATUSBAR}}                      <!-- 顶部状态栏（程序拼好的整段） -->

  <div class="card">
    <div class="title">你好</div>
    <div class="big">电量 {{BAT}}%</div>
  </div>

  {{DOTS}}                           <!-- 底部翻页圆点（程序拼好的整段） -->
</body>
</html>
```

- `<body class="{{THEME}}">`：`{{THEME}}` 会变成 `dark` 或 `light`，配合 CSS 里的 `body.dark` / `body.light` 实现主题。
- `{{STATUSBAR}}` 和 `{{DOTS}}` 是程序**已经拼成整段 HTML** 的复合令牌，直接放进去就行，建议每页都放，风格统一。

---

## 4. 能用什么、不能用什么（重要）

litehtml 不是浏览器，**限制比较多**，踩坑前先看这里：

| 能用 | 不能用 / 不可靠 |
|------|----------------|
| `block` / `inline-block` / `float` 布局 | ❌ **CSS Grid**（`display:grid`） |
| `table` / `tr` / `td` 表格布局 | ❌ **JavaScript**（完全没有） |
| `flex`（基本可用，复杂对齐不保证） | ❌ **CSS 动画 / transition**（不会动） |
| 颜色、圆角、边框、padding、margin | ❌ **图片**（`<img>`、`background-image` 都画不出来） |
| `position: absolute / relative` | ❌ **`var()` 自定义属性**（不可靠，用 class 切主题代替） |
| 文字、字号、粗细、对齐 | ❌ 元素级滚动容器（页面整体可竖向滚动，但内部 `overflow:scroll` 不可靠） |

几条最容易踩的：

- **没有图片**：图标请用 **CSS 画**（圆角块、边框拼形状）或**字体里的符号**。例如电池、信号格、开关都是纯 CSS 画的。
- **页面可以整体竖向滚动**：内容超过 480px 时可上下拖动；状态栏会固定在顶部。不要依赖元素内部滚动条，内容特别多时仍建议拆页。
- **想动起来只能靠数据刷新**：程序约 1 秒刷新一页（充电时更快）。没有 CSS 动画，"动画"只能通过令牌值每次变化来体现（比如充电时电池的流光就是程序每帧改一个 `left:%`）。
- **字体是设备自带的 CJK 字体**：中文、数字、常见标点都没问题；但很多符号字形缺失，会显示成**豆腐块**（□）。已知可用：`↑`(U+2191) `↓`(U+2193) `▲▼` `·` `℃` `•`；已知不可用：`◔` 等。拿不准的符号先在设备上试，或干脆用中文/CSS 画。
- **文字居中靠行高**：`vertical-align: middle` 按数字和汉字的视觉中线对齐，图标块和文字可以直接对齐。一个框同时定了 `height` 和 `line-height` 时，让 `line-height` 减去该字号的文字高度是偶数（设备字体 11→13、12→14、13→15、14→16、15→18、16→19、20→24、26→31），否则多出的 1 px 会让文字偏下半像素、取整后低 1 px。

---

## 5. 模板令牌 `{{令牌}}`

程序在渲染前把这些替换成实时值。**替换是单遍的**——复合令牌（已经是整段 HTML 的那些）里不会再二次替换，所以你不能在自己的 HTML 里"拼一个令牌名再让它展开"。

### 复合令牌（整段 HTML，直接放）

| 令牌 | 内容 |
|------|------|
| `{{STATUSBAR}}` | 顶部状态栏：时间（有未读短信时右侧由程序原生绘制一个主题第二强调色的信封图标）· 实时网速 · 制式 · 信号格 · 电池 · 电量 |
| `{{DOTS}}` | 底部翻页圆点，自动高亮当前页 |
| `{{SMSLIST}}` | 短信页折叠列表（整段）：最多 32 条，每条号码 / 时间 + 一行预览，是可点的 `act:sms:ID` 卡片（点开弹完整内容的二级页面并标为已读）；详情仅在正文真实超出视口时允许滚动，且只能点底部关闭按钮退出。未读条目红点 + 蓝色号码高亮；无短信显示「暂无短信」 |
| `{{SIGNALCARDS}}` | 默认第一页的完整信号区：运营商/制式/QCI/AMBR、载波卡片、信令解析附加指标、TA/品牌等高级卡片 |
| `{{NEIGHBORCARDS}}` | 首页邻小区入口和展开列表；信令解析关闭时为空，展开后每行显示 `PCI / 频段 / RSRP / RSRQ` |
| `{{CARRIERS}}` | 兼容旧模板的载波区：表头（组网模式 · L LTE + M NR 载波 · 总X MHz）+ 每载波一张卡片（频段·频宽 / EARFCN / PCI / RSRP / SINR，按质量上色，未激活置灰） |
| `{{NETSEG}}` | 选网方式分段控件（自动 / 5G SA / 5G NSA / 4G，当前模式高亮，支持点/滑） |
| `{{TOAST}}` | 居中提示气泡（无提示时为空），如"锁频成功" |
| `{{PINDOTS}}` | 锁屏键盘的 4 个 PIN 圆点（已输入的高亮，整段） |
| `{{LOCKMSG}}` | 解锁错误时的居中「密码错误」红色气泡（无错误为空，整段） |
| `{{LOCKAUX}}` | 锁屏键盘左下角辅助键：设置 PIN 时为「取消」键，解锁时为空 |

> 信号格条已并入 `{{STATUSBAR}}`；锁频的频段芯片由程序在二级弹窗里生成，页面只需用 `{{CURSA}}/{{CURNSA}}/{{CURLTE}}` 显示当前锁定摘要。

### 数据令牌（标量值）

| 令牌 | 含义 | 例 |
|------|------|----|
| `{{TIME}}` `{{DATE}}` | 时间 / 日期和星期 | `14:30` / `10月28日 周二` |
| `{{THEME}}` | body 类名：`dark` + 主题类 + 机型类 | `dark th-ocean dev-mu5250` |
| `{{SIGGAUGES}}` | 信号表盘卡片：SA 只有 NR 一排，纯 LTE 只有 LTE 一排，NSA / EN-DC 是 NR 和 LTE 两排 | 一段 HTML |
| `{{OPER}}` `{{NETTYPE}}` | 运营商（大陆四家中文）/ 制式 | `中国移动` / `SA` |
| `{{GEN}}` `{{GENCLASS}}` | 网络代际文字 / 样式类 | `5G+` / `g5p` |
| `{{BAT}}` `{{BATCLASS}}` | 电量% / 低电量类 | `100` / `low` |
| `{{QCI}}` `{{AMBR}}` | QoS QCI / 速率上限 | `6` / `3000/200 Mbps` |
| `{{RXSPEED}}` `{{TXSPEED}}` | 下载/上传速率（单位随设置） | `1.2 MB/s` |
| `{{RXBYTES}}` `{{TXBYTES}}` | 本次会话流量 | `120.5 MB` |
| `{{PCI}}` | 小区 PCI | `273` |
| `{{CELLID}}` `{{CELLBTN}}` | NR Cell ID(默认打码) / 显示按钮文字 | `8641413121` / `显示` |
| `{{SSID}}` `{{KEY}}` `{{KEYBTN}}` `{{ENC}}` | WiFi 名 / 密码(默认打码) / 显示按钮 / 加密 | |
| `{{CLIENTS}}` `{{CLIENTLIST}}` | 设备数 / 已连接设备列表(整段) | `2` |
| `{{DHCP_IP}}` `{{DHCP_POOL}}` `{{DHCP_LEASE}}` | 网关 / 地址池 / 租期 | `192.168.0.1` |
| `{{MODEL}}` `{{FW}}` `{{SWVER}}` `{{IMEI}}` `{{IMEIBTN}}` | 型号 / 固件 / 版本号 / IMEI(打码) / 显示按钮 | |
| `{{UPTIME}}` `{{UPSHORT}}` | 运行时间(完整 / 紧凑) | `2d 03:04:05` / `2天3时` |
| `{{CPUUSAGE}}` `{{CPUTEMP}}` `{{BATTEMP}}` | CPU占用% / CPU温度 / 电池温度 | `17` / `46` / `33` |
| `{{CHGV}}` `{{CHGI}}` | 充电器电压(V) / 电流(mA) | `4.79` / `456` |
| `{{BATV}}` `{{BATI}}` | 电池电压(V) / 电流(mA) | `4.50` / `59` |
| `{{PWR}}` `{{PWRLBL}}` | 功率(W) / 标签 | `2.2` / `充电`或`放电` |
| `{{MEM}}` `{{MEMDETAIL}}` | 内存占用% / 已用·总(MB) | `52` / `837/1590 MB` |
| `{{BRIGHT}}` | 屏幕亮度% | `80` |
| `{{SHEETCLASS}}` | 当前二级页是上下拉出来的面板时为 `sheet`，否则为空 | `sheet` / 空 |
| `{{AUTOBRIGHTCLASS}}` | 自动亮度开关的类（TopFlow 才可能是 `on`） | `on` / `off` |
| `{{BRIGHTBAR}}` | 自动亮度开启时为 `auto`，加在亮度滑条上把它隐藏，只留百分比 | `auto` / 空 |
| `{{AUTOOFF}}` | 自动息屏预设按钮组(整段，当前项高亮) | |
| `{{CURSA}}` `{{CURNSA}}` `{{CURLTE}}` | 各制式当前锁定频段摘要 | `已锁 n41 n78` |
| `{{USBPWRCLASS}}/{{USBPWRSTATE}}` `{{USBNETCLASS}}/{{USBNETSTATE}}` `{{BATPCTCLASS}}/{{BATPCTSTATE}}` `{{SBSPEEDCLASS}}/{{SBSPEEDSTATE}}` `{{TEXTADAPTCLASS}}/{{TEXTADAPTSTATE}}` `{{WIFICLASS}}/{{WIFISTATE}}` `{{WIFI24CLASS}}/{{WIFI24STATE}}` `{{WIFI5CLASS}}/{{WIFI5STATE}}` `{{PSMCLASS}}/{{PSMSTATE}}` `{{DPSCLASS}}/{{DPSSTATE}}` `{{NFCCLASS}}/{{NFCSTATE}}` `{{THEMESTATE}}`（主题名，如 `海洋`） `{{SPUNITCLASS}}/{{SPUNITSTATE}}` `{{LOCKCLASS}}/{{LOCKSTATE}}` | 各开关的类(`on`/`off`) 与状态文字 | `on` / `已开启` |
| `{{LOCKTITLE}}` | 锁屏键盘标题（设置时「设置锁屏密码」/ 解锁时「请输入锁屏密码」） | |
| `{{PAGE}}` `{{NPAGES}}` | 当前页 / 总页数 | `3` / `5` |

TopFlow（MU5252）专用令牌，在其他机型上为空或 `—`：

| 令牌 | 含义 | 例 |
|------|------|----|
| `{{MODEMCARDS}}` | 多基带卡片（整段）：每个基带一行，制式、运营商、在线状态、RSRP/SINR、频段、温度和上下行速率 | |
| `{{AGGCLASS}}` `{{AGGSTATE}}` `{{AGGSUM}}` | 聚合开关类 / 状态文字 / 首页摘要 | `on` / `在线 · 3/3 条链路` / `3/3 在线` |
| `{{AGGPATHS}}` | 聚合链路列表（整段）：在线点、名称、延迟、丢包 | |
| `{{FANSEG}}` `{{FANSTATE}}` | 风扇模式分段控件（原厂 / 曲线 / 常开）/ 转速和温度 | / `18% · 49°C` |
| `{{LIQSEG}}` `{{LIQSTATE}}` | 液冷模式分段控件（自动 / 低档 / 高档）/ 运行状态 | / `待机` |
| `{{TEMPS}}` | CPU 和各基带温度（整段表格） | |
| `{{CCEXTRA}}` | 控制中心第二排开关：聚合、风扇常开、液冷（整段） | |

在 TopFlow 上 `{{RXSPEED}}` / `{{TXSPEED}}` 是所有基带速率之和。

> 安全提示：`{{KEY}}`(WiFi 密码)、`{{CELLID}}`、`{{IMEI}}` **默认打码**(显示 `*`)，点对应"显示"动作才明文。请保留此行为。

---

## 6. 动作 `href="act:xxx"`

把链接 `href` 写成 `act:动作名`，点它触发动作并重绘当前页：

| 动作 | 效果 |
|------|------|
| `act:theme` | 按顺序切换四套主题（海洋 → 极光 → 日落 → 紫罗兰），并弹出主题名 |
| `act:revealkey` `act:revealcell` `act:revealimei` | 明文/打码 显示 WiFi 密码 / Cell ID / IMEI |
| `act:spunit` | 网速单位 Mbps(比特率) / MB/s(字节率) |
| `act:usbpower` | 切换 USB-C 供电方向：给 U60 充电 / U60 反向供电 |
| `act:usbnet` | 切换 USB 网络共享：开启时先走官方 `9057` RNDIS:ECM（无 ADB），若无 carrier 自动降到纯 ECM `90B1`；关闭时回到无网卡的 `1225 mass_storage` |
| `act:batpct` | 切换状态栏电池框内百分比显示 |
| `act:sbspeed` | 切换状态栏网速（时钟右侧一行，下行在左、上行在右，放不下时缩小字号、单位缩写；默认关） |
| `act:textadapt` | 切换文字自动变色（默认开，见“壁纸与液态玻璃”） |
| `act:glass` | 点在 `id="glass-bar"` 的条上设玻璃透明度；按住条拖动时跟手调节 |
| `act:glassblur` | 点在 `id="blur-bar"` 的条上设玻璃模糊程度（0–100 连续）；按住条拖动时跟手调节 |
| `act:glasstint:N` | 玻璃颜色：0 夜蓝、1 石墨、2 白雾、3 主题色 |
| `act:wifi` `act:nfc` | 切换 WiFi 总开关（两个主频段）/ NFC 碰一碰 |
| `act:wifi24` `act:wifi5` | 切换 2.4G / 5G 主频段（`ifconfig wlan0/wlan2 up/down`） |
| `act:psm` | 切换 WiFi 节能模式（`iw set power_save`，开=省电 / 关=高性能） |
| `act:dps` | 切换电源直供电（`zwrt_bsp.charger`，插电直供、不充放电池） |
| `act:bright` | 在亮度滑条上点/拖设置亮度（按命中位置算）；手动调节会关闭自动亮度 |
| `act:autobright` | TopFlow：切换自动亮度。亮屏且开启时由 datad 打开环境光传感器（`ambient_light.set`），按 `/state.ambient_light.lux` 平滑调节背光；熄屏或关闭时关掉传感器 |
| `act:autooff:<毫秒>` | 选自动息屏预设（`act:autooff:30000` 等，`0`=关） |
| `act:net:<值>` | 切选网模式（`WL_AND_5G`/`Only_5G`/`LTE_AND_5G`/`Only_LTE`） |
| `act:openmodal:sa` `:nsa` `:lte` | 打开对应制式的锁频二级弹窗 |
| `act:bsa:<频段>` `act:bnsa:<n>` `act:blte:<n>` | 在弹窗里勾选/取消某频段（仅本地缓存） |
| `act:mall` `act:minv` `act:mapply` | 弹窗：全选/全不选 · 反选 · 应用（应用才提交锁频） |
| `act:resetband` | 解锁所有频段并恢复默认 |
| `act:locktoggle` | 锁屏开关：关→开进入 PIN 设置键盘；开→关清除密码、关闭锁屏 |
| `act:pin:<0-9>` | 锁屏键盘按下一位数字（满 4 位自动校验/保存） |
| `act:pin:del` | 锁屏键盘删除最后一位 |
| `act:lockcancel` | 放弃 PIN 设置（仅设置键盘左下角） |
| `act:poweroff` `act:reboot` `act:close` `act:menu` | 关机 / 重启 / 关菜单 / 开菜单（一般只在 `menu.html`） |
| `act:sub:<html>` | 打开 `ui/subpages/` 下的内置二级页面 |
| `act:func:<html>` | 打开 `ui/functions/` 下的自定义二级页面 |
| `act:backfunc` | 从二级页面返回顶层页 |
| `act:neighbors` | 首页展开 / 收起邻小区列表 |
| `act:stpage` | 打开测速二级页（只有测速后端存在时更多功能页才显示入口） |
| `act:agg` | TopFlow：聚合开关。开启点一下即可；关闭会断开聚合链路，第一下只提示“再点一次”，4 秒内再点才执行 |
| `act:fan:<模式>` `act:fanon` | TopFlow：风扇模式 `automatic` / `custom` / `always_on`；`fanon` 在常开和曲线之间切换 |
| `act:liq:<模式>` `act:liqon` | TopFlow：液冷模式 `automatic` / `low` / `high`；`liqon` 在高档和自动之间切换 |

`act:usbpower` 和 `act:usbnet` 在 TopFlow 上不做任何事（它不支持反向供电和 USB 网络共享）。

例：一个开关按钮

```html
<a href="act:spunit" class="sw {{SPUNITCLASS}}"><span class="kn"></span></a>
```

`{{SPUNITCLASS}}` 是 `on`/`off`，配合 CSS 的 `.sw.on` / `.sw.off` 把圆钮画在右/左，就是一个拨动开关。

> 滑条/分段控件类（亮度 `id="bright-bar"`、选网 `id="netseg"`）由程序按元素 `id` 定位并处理点击/拖动，所以这些元素**要带对应的 `id`**。

---

## 7. 主题与原生绘制

默认界面照 U30 Pro 屏幕的风格：深蓝底带一圈柔光、半透明卡片、两种强调色。共有四套主题，都是深色，区别只在强调色：

| 主题 | 类名 | 强调色 a / b |
|------|------|------|
| 海洋 | `th-ocean` | `#38D0FF` / `#FF7AC8` |
| 极光 | `th-aurora` | `#4ADE80` / `#FFC857` |
| 日落 | `th-sunset` | `#FF9F43` / `#6C8CFF` |
| 紫罗兰 | `th-violet` | `#A78BFA` / `#2DD4BF` |

`<body class="{{THEME}}">` 会变成 `dark th-ocean` 这样的两个类。因为 `var()` 不可靠，强调色按主题类各写一条规则：

```css
.th-ocean  .seg-on { background: #38d0ff; color: #0b1220; }
.th-aurora .seg-on { background: #4ade80; color: #0b1220; }
```

背景支持 `linear-gradient` 和 `radial-gradient`（默认页面底色就是一个径向渐变）。状态栏由程序原生绘制在 `#070C15` 上，和渐变边缘同色。
旧版保存的“浅色模式”升级后会落到“极光”。

有些图形 CSS 画不出来，程序会在页面渲染完后，按元素 `id` 找到占位框，把图形原生画进去。占位框只需要给出位置和大小：

| `id` | 画什么 |
|------|------|
| `chart-cpu` `chart-mem` `chart-net` `chart-batt` | 折线图（图表页） |
| `gauge-nr-rsrp` `gauge-nr-sinr` `gauge-lte-rsrp` `gauge-lte-sinr` | 270° 圆弧表盘：NR / LTE 那一路的 RSRP 和 SINR，中间是数值，开口处是名称；那一路没有读数时显示 `--`。`{{SIGGAUGES}}` 会按制式生成这些占位框 |
| `gauge-rsrp` `gauge-sinr` | 同上，但只画服务小区（NR 在用时是 NR，否则 LTE），给自定义模板用 |
| `home-ring` | 电量环（绕首页时钟），充电或电量 > 30% 为绿色，≤ 30% 黄色，≤ 15% 红色 |
| `home-bars` `home-wifi` `home-bolt` | 首页的信号格、Wi-Fi 图标、充电闪电（只在充电时画） |
| `fic-wifi` `fic-sms` `fic-cell` `fic-lock` `fic-speed` `fic-custom-N` | 功能列表左侧圆圈里的线条图标（主题色），自定义页面是四宫格 |
| `cc-wifi` `cc-nfc` `cc-dps` `cc-theme` | 控制中心的圆形开关图标，开着时圆盘是主题色、图标深色 |
| `cc-agg` `cc-fan` `cc-liq` | TopFlow 控制中心的聚合、风扇常开、液冷图标 |

渐变写进帧缓冲时带 4×4 有序抖动：屏幕是 RGB565，直接量化会出现一条条色带。

### 壁纸与液态玻璃

插件把壁纸存成安装目录下的 `wallpaper.jpg`（320×480），把设置存成 `wallpaper.conf`：`glass=` 按钮透明度 0–100，`dim=` 壁纸暗度 0–90，`tint=` 玻璃颜色（同 `act:glasstint`），`blur=` 模糊程度 0–100（默认 50，有壁纸时才有作用），`live=` 动态壁纸在哪播放（0 不播，1 锁屏，2 锁屏和主页，默认 2）。文件改了约 1 秒内生效，删掉 `wallpaper.jpg` 就回到主题底色。屏幕上的设置页“玻璃效果”（`subpages/glass.html`）也能调透明度、模糊程度和颜色，会改写 `wallpaper.conf`。

**动态壁纸**：插件里选视频时，取前 20 秒按视频自己的帧率（最高 30 帧）裁成 320×480，存成 `wallpaper.live`（格式见 `src/livewp.c`），第一帧另存为 `wallpaper.jpg`。有 `wallpaper.live` 时它优先。锁屏和主页没人操作时播放，一碰屏幕就停在当前这一帧，其他页面也用这一帧当壁纸，回到锁屏或主页后接着播。页面照常画在壁纸上，播放时每一帧按记下的透出比例把玻璃和壁纸换成当前帧，所以字的颜色不随画面闪动。帧率受屏幕传输限制，U60 上约 27 帧。

玻璃设置的令牌：`{{GLASS}}`（透明度数字）、`{{GLASSBLUR}}`（模糊程度数字）、`{{GLASSTINT}}`（颜色名）、`{{GLASSTINTSEG}}`（整排颜色选项）。

CSS 里有三个内建的背景图：

- `url(wallpaper)`：壁纸。设了壁纸时 `<body>` 会多一个 `wp` 类，默认样式写的是 `body.wp { background: #070c15 url(wallpaper); }`。壁纸固定在屏幕上：翻页、滚动、拉面板、右滑返回时只有页面内容在动，壁纸和状态栏下面的那一截都不动。程序记得每个像素透出了多少壁纸和玻璃，移动的每一帧都按它当时所在的位置重新透出：玻璃框、`rgba()` 半透明底（图表、滑块轨道、分隔线）和文字边缘都跟着固定的壁纸走。
- `url(glass)`：液态玻璃。框里是它下面的壁纸（没有壁纸时是页面底色）经过模糊、边缘像透镜一样往里弯，再按透明度混入深色面板，边缘有一圈上亮下暗的高光。边框写成 `transparent`，高光由程序画。
- `url(glass-thin)`：更薄的玻璃，面板浓度少约三分之一，透出的壁纸更亮，适合主页表盘这类大面积区域。

```css
.mybtn { border-radius: 16px; background: url(glass); border: 1px solid transparent; }
```

玻璃直接取壁纸，不取叠在下面的其他框，所以别把玻璃框放进另一个玻璃框里；卡片里的小按钮照旧用 `rgba()` 半透明色。玻璃会限制透出的壁纸亮度：深色玻璃不会透得太亮，白雾玻璃不会透得太暗，所以玻璃上的字不管壁纸怎样都看得清。

设了壁纸时，原本直接压在壁纸上的字默认都垫了玻璃：状态栏是一条玻璃胶囊，主页表盘里是一块薄玻璃，“N 台”和控制中心图标下的文字是玻璃小胶囊，短信、锁频等对话框也是玻璃。自己写页面时，壁纸上的字也建议这样处理（`.wp` 下给它加 `url(glass)` 底）。

**文字自动变色**（设置页开关，默认开）：有壁纸时按文字所在的底来处理，不逐字变色。
- 玻璃上的字保持设计颜色，偏暗的灰字会提亮；白雾玻璃上换成同色相的深色，状态栏、解锁胶囊和控制中心未开启的图标也一样。
- 仍然直接写在壁纸上的字（页面标题等）加粗一档，字后面的壁纸轻微抹平细节（不改亮度、边缘渐隐），一行字统一用冰白色；只有这一行壁纸极亮（平均亮度超过 200 且几乎没有暗色）时整行换成深石板色。底部翻页小圆点同理。
- 开关、选中的分段、彩色标签这类不透明底上的字保持原样。

关掉开关或没有壁纸时，只保留旧规则：浅底上的浅色字压深，深底上的深色字提亮。

### 上下拉面板

在顶层页面上，屏幕任意位置竖直往下拉打开**控制中心** `subpages/control.html`，往上拉打开**短信列表** `subpages/sms.html`。页面能滚动时，滚到顶端才下拉出控制中心、滚到底端才上拉出短信，其余时候照常滚动。
面板就是普通二级页，点按、滚动照常。作为面板拉出来时 `{{SHEETCLASS}}` 为 `sheet`，样式据此隐藏返回键、显示收起提示：控制中心任意位置上拉收起，短信列表滚到顶后下拉收起；从别的页面用 `act:sub:` 打开时照常显示返回键。拉动时面板跟着手指走，松手时拉过 60 px 就滑到底，否则弹回。

想在宿主机上看效果：`bash tests/ui/preview.sh 输出目录 [主题] [状态.json]`，会把每一页渲染成 PNG 并拼一张总览图（需要 Linux、gcc/g++、Python Pillow 和中文字体）。

---

## 8. 部署与调试

```sh
# 推送你的界面（程序会自动热重载，无需重启）
adb shell 'mkdir -p /data/plugins/zwrt-devui/ui'
adb push 你的页面.html /data/plugins/zwrt-devui/ui/
adb push style.css     /data/plugins/zwrt-devui/ui/
```

注意（Windows 用户）：用 Git-Bash/MSYS 跑 `adb push` 时，`/data/plugins/zwrt-devui/ui/` 这种路径可能被错误地翻译成本地路径导致卡住。**建议用 PowerShell 跑 adb**，或一个文件一个文件地推。

调试技巧：

- 改完看不到效果？确认推到的是 `/data/plugins/zwrt-devui/ui/`，并且程序在运行（`pidof zwrt-devui`）。
- 排版乱 / 下面看不到？先确认页面高度和底部留白是否合理；长页面可以竖向滚动，但内容特别多时仍建议精简或拆页。
- 出现豆腐块 □？是字体缺这个符号字形，换中文或 CSS 画。
- 想从零做：复制现成的 `01-signal.html` 改，是最快的起点。

---

## 9. 一个完整示例：加一页"关于"

新建 `/data/plugins/zwrt-devui/ui/04-about.html`：

```html
<!DOCTYPE html>
<html lang="zh-CN">
<head><meta charset="UTF-8"><link rel="stylesheet" href="style.css"></head>
<body class="{{THEME}}">
  {{STATUSBAR}}
  <div class="card">
    <div class="title">关于本机</div>
    <table>
      <tr><td class="kv-l">型号</td><td class="val">{{MODEL}}</td></tr>
      <tr><td class="kv-l">固件</td><td class="val">{{FW}}</td></tr>
      <tr><td class="kv-l">运行</td><td class="val">{{UPTIME}}</td></tr>
    </table>
  </div>
  <div class="card">
    <div class="big">{{OPER}} · {{GEN}}</div>
    <div class="sec">第 {{PAGE}} / {{NPAGES}} 页</div>
  </div>
  {{DOTS}}
</body>
</html>
```

```sh
adb push 04-about.html /data/plugins/zwrt-devui/ui/
```

滑到第 4 页就能看到，圆点也自动变 4 个。完成。


## Optional speedtest page

The "更多功能" page may show a `网络测速` tile when `/data/plugins/better-speedtest/better-speedtest` is installed and executable. The tile opens `ui/subpages/speedtest.html`, which uses DevUI-native drawing for the gauge and line charts. Duration `0` is loop mode: the button turns into a loop start action and the page shows an orange traffic warning until the user taps stop. Locked preview mode does not expose speedtest controls; backend detection still runs for after unlock.
