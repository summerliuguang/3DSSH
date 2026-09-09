<p align="center">
  <a href="README.md">English</a> · <b>中文</b>
</p>

<h1 align="center">DSSH+</h1>

<p align="center">
  <img src="icon.png" alt="DSSH+ icon" width="96" height="96">
</p>

<p align="center">
  <b>Nintendo 3DS 中文 SSH 客户端 — 拼音输入法 · 语音输入 · ANSI 终端</b><br>
  上屏是 citro2d 渲染的 ANSI 终端 · 下屏是自绘软键盘 · libssh2 + mbedTLS
  RSA 公钥认证<br>
  按 <b>START</b> 直接对 Claude Code 说中文，其余内容用拼音键盘补打<br>
  从 3DS 直连服务器跑 tmux + claude-code——窝在沙发上也能写代码，
  笔记本都不用开
</p>

<p align="center">
  <img alt="platform" src="https://img.shields.io/badge/platform-Nintendo%203DS-FE0016?logo=nintendo3ds&logoColor=white">
  <img alt="license" src="https://img.shields.io/badge/license-MIT-blue">
  <img alt="build" src="https://img.shields.io/badge/build-devkitARM-orange">
</p>

<p align="center">
  <img src="docs/media/preview.gif" alt="DSSH+ 实机演示" width="540"><br>
  <sub>实机 New 2DS XL · 上屏 ANSI 终端 · 下屏软键盘 + 时钟 + 螃蟹</sub>
</p>

<p align="center">
  <a href="https://github.com/Fishason/DSSH/releases/latest/download/demo.mp4">
    完整 1m42s 实机演示（10 MB MP4）
  </a>
</p>

<p align="center">
  <img src="docs/media/poster.jpg" alt="实机：在 Claude Code 中用拼音输入法输入中文" width="720"><br>
  <sub>实机 New 2DS XL · 上屏正在向 Claude Code 输入「你好啊！请问您是谁，你可以做什么」 ·
  下屏字母页 + CHN 模式 + 按住 Shift</sub>
</p>

---

## 关于本 Fork

本仓库是 [Fishason/DSSH](https://github.com/Fishason/DSSH) 的 fork，
名为 **DSSH+**；而 Fishason/DSSH 又源自最早的
[skmtrd/3dssh](https://github.com/skmtrd/3dssh)。上游 DSSH 贡献了拼音
输入法、软键盘和云端语音输入，**本 fork 在此基础上继续演进**，方向是
让 3DS 不连电脑也能自己搞定一切：

- **机内设置页（v1.2）**——直接在 3DS 上编辑最多 4 组服务器配置
  （支持密码登录）和语音 API 地址，SAVE 写回 SD 卡，不再需要
  在电脑上手工准备 `config.ini`。
- **多窗口（v1.3）**——最多同时连接 4 台服务器，切换瞬时完成，
  后台会话保持在线。
- **HTTP 语音 API（v1.2）**——START 录音可以改为直接 POST 给
  自建的转文字服务，这条路不需要 OpenRouter key。
- **独立 CIA 身份**——本 fork 的 CIA 以 **DSSH+** 名义安装
  （UniqueId `0xFF55D`，蓝色双箭头图标），与上游 DSSH（`0xFF55C`）
  **互不覆盖、可共存**。
- **真机修复**——语音后端初始化与错误原因显示、全屏 TUI 的滚轮
  目标位置、设置编辑栏不再遮挡键盘数字键等（见
  [commit 历史](../../commits)）。

以下文档均针对**本 fork**，与上游未必一致。

## 功能

*终端与输入*

- **完整 ANSI/VT100 终端**：tmux 状态栏、claude-code 转圈动画、
  制表线边框、256 色、真彩色、盲文字符——全部正常渲染。
- **中文渲染**：内置 Zpix 12px 像素字体（覆盖 21,000+ CJK 统一
  汉字）+ Terminus 6×12 ASCII 字体，中英混排基线对齐。
- **自绘软键盘**：iOS 风格 3px 圆角按键，带按下动画和按键音效；
  字母 / 符号两页。
- **拼音输入法**：基于 rime-ice 前 30 万条词典，支持声母缩写
  （`nh` → 你好）、前缀回退（多打的字母自动变红）和候选词光标。
- **物理键全映射**：D-pad 方向键，L/X/Y 为按住生效的修饰键
  （Shift / Alt / Ctrl），Circle Pad 滚动历史 / 模拟鼠标滚轮。

*语音*

- **语音输入（v1.0）**：按 **START** 说一句中文，再按一次结束，
  一两秒后转写文字直接进 SSH 终端。
- **语音问 AI（v1.1）**：按住 **L + START** 用语音向 DeepSeek-Chat
  提问，答案在下屏弹窗里带 Markdown 样式渲染（标题黄色、行内代码
  青色、列表符号等），顶屏 SSH 会话完全不受打扰。
- **HTTP 语音 API（v1.2，本 fork 新增）**：START 录音可改为直接
  POST 给任何返回 `{"text": "..."}` 的自建转写端点。

*连接与认证*

- **RSA-4096 公钥认证**：libssh2 + mbedTLS，私钥从 SD 卡读取；
  也支持**密码登录**（v1.2 起）。
- **原生 Tailscale 传输**：可选让 3DS 加入 tailnet，走直连 UDP、
  Tailscale Peer Relay 或 DERP，无需安装 Go 或运行 `tailscaled`。

*本 fork 重点*

- **机内多服务器设置页（v1.2）**：键盘右下角的 **SET** 按钮打开
  设置页——最多 4 台服务器，HOST/PORT/USER 直改，**密码登录**或
  RSA 公钥二选一，语音 API 直填；SAVE 写回 SD 卡，RECONNECT
  一键重连。
- **多窗口（v1.3）**：SET 旁边的 **WIN** 按钮在已配置的服务器窗口
  间循环切换。后台窗口**保持在线**（tmux / vim 照常运行），切换
  瞬时完成、互不干扰。每个窗口一个独立终端（约 0.6 MB，含 500 行
  历史缓冲），4 窗口总增量约 2.5 MB，对 64 MB 内存的 3DS 毫无压力。
- **独立身份（DSSH+）**：与上游 DSSH 可共存，见
  [关于本 Fork](#关于本-fork)。

*其他*

- **Anthropic 红螃蟹吉祥物**：在底屏一行来回奔跑，点它会躲开 🦀。
- **隐藏 debug 页面**：双击右上 ENG/CHN 徽章进入，可看 SSH 字节流、
  当前语音后端、按键速查表和螃蟹开关。

## 目录

- [关于本 Fork](#关于本-fork)
- [安装](#安装)
- [服务器侧准备](#服务器侧准备一次性)
- [配置 config.ini](#配置-configini)
- [语音输入](#语音输入)
- [按键说明](#按键说明)
- [输入法用法](#输入法用法)
- [Debug 页面](#debug-页面)
- [从源码构建](#从源码构建)
- [项目结构](#项目结构)
- [致谢](#致谢)
- [License](#license)

---

## 安装

DSSH+ 跑在**已完成自定义固件的 3DS / 2DS / New 3DS** 上，需要
Homebrew Launcher（HBL）或 FBI 一类的 CIA 安装器。

### 方案 A — `.cia` 安装（推荐）

1. 从本仓库 [Releases](../../releases) 下载 `DSSH.cia`（约 14 MB）。
2. 拷到 SD 卡，路径任意（比如 `/cias/DSSH.cia`）。
3. 打开 FBI → SD → 选中 `DSSH.cia` → `Install CIA`。
4. HOME 菜单出现蓝色双箭头的 **DSSH+** 图标。它是独立的
   title（UniqueId `0xFF55D`），已安装的上游 DSSH（`0xFF55C`）
   不受影响，两者可共存。

### 方案 B — `.3dsx` 直跑

1. 从 Releases 下载 `3dssh.3dsx`。
2. 拷到 SD 卡 `/3ds/3dssh/3dssh.3dsx`。
3. 打开 HBL → 选 DSSH。

### 方案 C — `3dslink` WiFi 推送（开发用）

```bash
# 3DS 上进 HBL 后按 Y，进入 "Waiting for 3dslink..."
3dslink -a <3DS局域网IP> 3dssh.3dsx
```

---

## 服务器侧准备（一次性）

3DS 的 libssh2 使用 mbedTLS 加密后端，**编译时禁用了 ed25519**。
所以要为 3DS 单独生成一把 RSA-4096 密钥，PC 上原有的 ed25519
密钥不受影响、照常使用。

> ⚠️ **`-m PEM` 参数必须加。** devkitPro 仓库里的 `3ds-mbedtls`
> 固定在 2.28.x，**只能解析传统 PEM 格式**的私钥（首行
> `-----BEGIN RSA PRIVATE KEY-----`）。Ubuntu 18.04+ / macOS 的
> 新版 `ssh-keygen` 默认输出**新 OpenSSH 格式**（首行
> `-----BEGIN OPENSSH PRIVATE KEY-----`），mbedtls 2.28 解析不了
> ——不加 `-m PEM`，DSSH 会在握手时报 "auth failed"。

```bash
# 1. 在 PC 上生成 3DS 专用 RSA 密钥。-m PEM 强制传统 PEM 格式，
#    3DS 上的 mbedtls 才能解析。
ssh-keygen -t rsa -b 4096 -m PEM -f ~/.ssh/id_rsa_3ds -C "3ds-ssh-client"

# 2. 把公钥添加到服务器的 authorized_keys
ssh-copy-id -i ~/.ssh/id_rsa_3ds.pub user@your-server.example.com

# 3. 在 PC 上验证新密钥可用
ssh -i ~/.ssh/id_rsa_3ds user@your-server.example.com 'echo OK'
```

**已有 OpenSSH 格式的私钥？** 原地转换格式即可，公钥不变，
**不用**重新添加到服务器：

```bash
ssh-keygen -p -m PEM -f ~/.ssh/id_rsa_3ds
# 输入旧 passphrase（没有就直接回车）
# 转换后文件首行应为：-----BEGIN RSA PRIVATE KEY-----
```

可以用 `head -1 ~/.ssh/id_rsa_3ds` 检查首行确认格式。

**安全建议**：在服务器的 `~/.ssh/authorized_keys` 里给这行公钥加上
`from="<家里公网 IP>"` 前缀——这样即使 SD 卡丢失，这把钥匙也只能
从你家网络登入。

把**私钥** `~/.ssh/id_rsa_3ds` 拷到 SD 卡的 `/3ds/3dssh/id_rsa`
（无论以 `.3dsx` 还是 `.cia` 安装，配置和密钥都固定从
`sdmc:/3ds/3dssh/` 读取）。

> ⚠️ SD 卡上的私钥是明文。谁拿到 SD 卡，谁就能登你的服务器。
> 务必在 `authorized_keys` 里加 `from="..."` IP 限制或
> `command="..."` 命令限制。

密码登录是公钥之外的另一种选择——在设置页或 `config.ini` 里配置
（见下节）。密码同样明文存放在 SD 卡上，带 `from=` 限制的公钥
仍是更安全的默认选择。

---

## 配置 config.ini

**v1.2 起可以直接在 3DS 上完成全部配置**：点键盘右下角的 **SET**
按钮打开设置页（最多 4 台服务器，`< SRV n/4 >` 切换，浏览到已配置
的槽位即选中它；点某一行进入编辑，**A** 确认 / **B** 退格 /
**SELECT** 取消），改完点 **SAVE** 写回 SD 卡，点 **RECONNECT**
立即重连。

手工编辑仍然支持——把仓库里的
`sd_template/3ds/3dssh/config.ini.example` 拷到 SD 卡的
`/3ds/3dssh/config.ini`，按自己的服务器修改：

```ini
# 服务器 1..4；下面 server1 用 RSA 公钥，server2 用密码
server1_host = your-server.example.com
server1_port = 22
server1_user = ubuntu
server1_auth = key
server1_key_path = sdmc:/3ds/3dssh/id_rsa
server1_passphrase =

server2_host = 192.0.2.10
server2_port = 22
server2_user = root
server2_auth = password
server2_password = change-me

# 开机连接哪台（从 1 数）；在设置页浏览到某台也等于选中它
active_server = 1

# 可选：HTTP 语音转文字端点，见「语音输入」一节
voice_api_url =

# 可选：自动解锁当前 SSH 用户的 macOS 登录钥匙串
macos_keychain_password =

# 可选：经 tailnet 连接 SSH
tailscale_auth_key =
tailscale_hostname = dssh-3ds
tailscale_state = sdmc:/3ds/3dssh/tailscale.state
tailscale_control_url = https://controlplane.tailscale.com
```

| 字段 | 说明 |
|------|------|
| `serverN_host` / `serverN_port` / `serverN_user` | 第 N 台服务器的地址、端口（默认 22）、用户名 |
| `serverN_auth` | `key`（默认，RSA 公钥）或 `password`（密码登录） |
| `serverN_key_path` / `serverN_passphrase` | RSA 私钥路径与口令；`sdmc:/...` 是 3DS 标准 SD 路径前缀 |
| `serverN_password` | 密码登录的口令（明文存 SD 卡，注意物理安全） |
| `active_server` | 开机连接的服务器编号（从 1 数） |
| `voice_api_url` | HTTP 语音转文字端点；留空走经典 SSH shim 通道 |
| `macos_keychain_password` | 可选的 macOS 登录密码；填写后为当前 SSH 用户启用自动解锁 |
| `tailscale_auth_key` | 注册用的 auth key；填写后自动启用 Tailscale |
| `tailscale_hostname` | 在 tailnet 中显示的 3DS 设备名 |
| `tailscale_state` | 持久化机器 / WireGuard / DISCO 身份的文件 |
| `tailscale_control_url` | 协调服务器，默认用 Tailscale 官方 SaaS |

> 旧的平铺写法（`host =` / `port =` / `user =` / `key_path =` /
> `passphrase =`）仍然兼容，等价于只配置了 server1。

填写 `macos_keychain_password` 后，DSSH 会等当前交互式 PTY shell
初始化完成，然后执行
`/usr/bin/security unlock-keychain "$HOME/Library/Keychains/login.keychain-db"`。
只有检测到 macOS 输出 `password to unlock` 后，才会通过 PTY 发送
密码——不用 `-p` 参数，密码不会进入远端进程参数或 shell history。
这满足 macOS 钥匙串对交互式会话的要求，fish / zsh / bash 均可。
发送之后 DSSH 会清掉内存里的密码副本。即使解锁失败，普通 SSH
shell 也不受影响，只会显示一条警告；之后再启动 `claude`，它就能
读取登录钥匙串里已有的凭据。

解锁成功时不显示任何诊断日志，DSSH 会清掉 bootstrap 输出再让 fish
重绘提示符；失败时只保留一条带 unlock / verify 状态码的简短警告。
密码本身永远不会显示。

密码含 `#` 或首尾空格时请加引号：

```ini
macos_keychain_password = "my # password"
```

> ⚠️ 此选项会把 **macOS 登录密码明文存在可移除的 SD 卡上**，比一把
> 专用 SSH 密钥更敏感。条件允许时请使用专门的 macOS 账户，并妥善
> 保管 SD 卡；不能接受这个风险就不要填这个字段。

### 原生 Tailscale 传输

Tailscale 支持由 [`cadl/libts3ds`](https://github.com/cadl/libts3ds)
提供——一个从 MicroLink 派生、面向 Nintendo 3DS 的实验性原生 C
客户端。它不是官方的 Go `tailscale/libtailscale`，也不运行
`tailscaled`。DSSH 通过 `libts3ds` git submodule 把源码固定在经过
真机验证的
[`libts3ds-v0.1.0`](https://github.com/cadl/libts3ds/releases/tag/libts3ds-v0.1.0)
版本；顶层 `make` 会自动先构建这个静态库。

启用后，SSH 走 libts3ds 内部的私有 lwIP TCP 栈，可选直连 UDP、
Tailscale Peer Relay 或 DERP。默认构建自动选路；诊断用途可以在
编译时用 `DSSH_TAILSCALE_PATH=direct`、`peer-relay` 或 `derp`
固定数据路径。

没有 auth key 时，只要 state 文件存在，Tailscale 也会自动启动。
`tailscale_hostname` 默认 `dssh-3ds`，`tailscale_state` 默认
`sdmc:/3ds/3dssh/tailscale.state`。注册成功后 auth key 可以留在
配置里；担心 SD 卡丢失导致泄露的话也可以删掉。

state 文件包含机器、WireGuard 和 DISCO 私钥身份，必须与 auth key
一样按敏感信息对待。本集成是非官方社区项目，与 Tailscale Inc.
无隶属或背书关系。

最终 SD 卡布局：

```
sdmc:/3ds/3dssh/
├── config.ini
├── id_rsa
└── tailscale.state            # Tailscale 注册后生成
```

---

## 语音输入

按 **START** 开始录音，说一句中文，再按 **START** 结束——一两秒后，
转写出的文字就像手打一样出现在 SSH 终端里。整句话只用嘴说，
软键盘都可以不碰。

3DS 用内置麦克风录 16 kHz 单声道 PCM（单次录音上限约 30 秒：1 MB
缓冲约能装 32 秒，软件定时器会在此之前一点停住）。录音交给以下
三种后端之一处理：

| 配置 | 传输路径 | 转写 | 适合 |
|---|---|---|---|
| 设置了 `voice_api_url` | WAV 经 HTTP POST 到自建转写服务 | 任何返回 `{"text": "..."}` 的服务 | 自建 / 局域网 ASR（如 MiMo ASR），无需云端 key |
| `voice_api_url` 留空（默认） | 同一 SSH 会话的第二个 libssh2 channel → `dssh-whisper-shim` | OpenRouter Whisper Large V3 Turbo（云端） | 绝大多数用户——零新端口、零防火墙改动 |
| 同上，服务器执行 `dssh-whisper switch local` 后 | SSH shim | 服务器本地 whisper.cpp | 离线 / 隐私需求 |

**状态指示**（状态条左侧的语音槽位）：

- 🔴 **REC**（红色脉冲）— 录音中
- ⠋⠙⠹⠸（青色转圈）— 上传 + 转写中
- 红色错误文字（约 6 秒）— 显示**具体失败原因**（如
  `http timeout`、`open ctx 0x...`、`mic init`）；再按 START 重试

### HTTP 转写端点（自建服务器，本 fork 新增）

不走 SSH shim 时，DSSH 可以把 START 录音直接 POST 给任意 HTTP
转文字端点：请求体是 16 kHz PCM16 单声道 WAV，响应 JSON 里的
`text` 字段就是转写结果。在设置页或 config.ini 里配置：

```ini
voice_api_url = http://192.0.2.10:29016/api/stt
```

> **务必用 http:// 而不是 https://**：3DS 系统 HTTP 模块最高只支持
> TLS 1.0（老机型 / 老固件），而现代 nginx 只收 TLS 1.2+，所以
> https 端点在 3DS 上连握手都完不成（界面报错、请求根本没到服务器）。
> 3DS 侧虽然不校验自签证书（`SSLCOPT_DisableVerify`），但握手这一步
> 就过不去，放行证书也用不上——实践中请直接用 http。

- 转写结果以与 shim 通道相同的打字机效果打进终端
- 上传在后台线程执行，UI 不卡
- 设置页 SAVE 后立即生效；debug 页会显示当前后端
  （`VOICE: HTTP API / SSH shim`）
- 编译期可把局域网端点烘焙成默认值（不进仓库）：
  `make DSSH_VOICE_API_DEFAULT=http://your-server:29016/api/stt`
- 参考实现：mimo-voice-hub 的 `/api/stt`（小米 MiMo ASR），配套
  浏览器页面 `/stt` 可用麦克风直接试转写；出错时 debug 页的 hex
  区能看到服务器的响应体
- **`L+START` 的 AI 问答不走这条通道**——它始终经过 SSH shim
  （只有 shim 认识 DeepSeek）

### 云端后端（默认）—— 经 SSH shim 调 OpenRouter Whisper

经典路径：录音通过**同一 SSH 会话的第二个 channel** 传给服务器上的
一个小 shim（零新端口、零新认证、零防火墙改动），shim 再调用
OpenRouter 的 Whisper Large V3 Turbo（$0.04 / 音频小时，个人用量
每月几美分）。

语音功能在服务器端需要**两把 API key**：

| Key | 申请地址 | 用途 | 是否必需 |
|---|---|---|---|
| **OpenRouter** | [openrouter.ai/settings/keys](https://openrouter.ai/settings/keys) | Whisper Large V3 Turbo（语音→文字）| **必需**——语音输入（START）和 AI 问答（L+START）都用 |
| **DeepSeek** | [platform.deepseek.com/api_keys](https://platform.deepseek.com/api_keys) | DeepSeek-Chat（AI 回答）| 可选——只有 L+START AI 问答用；纯语音输入不需要 |

两把 key 加起来每月成本几美分；两家注册都送少量免费额度，足够
上手测试。

#### 一条命令安装

SSH 进服务器后执行：

```bash
git clone https://github.com/summerliuguang/3DSSH.git ~/dssh-repo
bash ~/dssh-repo/tools/install_whisper_api.sh
```

安装脚本会交互式提示输入两把 key：

```
▶ checking prerequisites...
✓ python3 3.10
▶ installing daemon + shim + dssh-whisper CLI ...
▶ writing track config...

Need your OpenRouter API key (https://openrouter.ai/settings/keys).
Used for Whisper transcription.  Looks like:  sk-or-v1-...
Paste your OpenRouter key (or empty to skip): █

✓ api-key saved at /home/you/.config/dssh-whisper/api-key (chmod 0600)

Optional: DeepSeek API key (https://platform.deepseek.com/api_keys).
Used only by the L+START voice AI-ask modal — skip if you only
want plain voice IME.  Looks like:  sk-...
Paste your DeepSeek key (or empty to skip): █

✓ deepseek-key saved at /home/you/.config/dssh-whisper/deepseek-key (chmod 0600)
✓ dssh-whisper-api installed (lightweight, OpenRouter Whisper Turbo).
```

**非交互式**安装（适合 CI / Ansible / 重新部署）：

```bash
OPENROUTER_API_KEY="sk-or-v1-..." \
DEEPSEEK_API_KEY="sk-..." \
bash ~/dssh-repo/tools/install_whisper_api.sh
```

#### 验证安装

```bash
$ dssh-whisper status
active track: api
(API-only install — no local daemon)
api-key:  ✓ /home/you/.config/dssh-whisper/api-key
```

3DS 每次按 START 都通过 SSH-exec 调用
`~/.local/bin/dssh-whisper-shim`，服务器端装完即长期使用，无需
其他操作。

#### 安装后服务器上会多出这些文件

```
~/.config/dssh-whisper/
├── track            # "api" 或 "local"
├── api-key          # chmod 0600 — OpenRouter
└── deepseek-key     # chmod 0600 — DeepSeek（可选）
~/.local/bin/
├── dssh-whisper          # CLI（start/stop/status/switch/uninstall）
└── dssh-whisper-shim     # 3DS 通过 SSH-exec 调用的入口
~/.local/share/dssh-whisper/
└── whisper_shim.py       # 与两个 API 通信的实际逻辑
```

总占用约 30 KB：无 daemon、无模型、无需运维。

#### 更换 key

key 泄露、额度用完或更换服务商时，直接覆写文件即可：

```bash
echo 'sk-or-v1-NEW_KEY' > ~/.config/dssh-whisper/api-key
chmod 0600 ~/.config/dssh-whisper/api-key
# 无需重启任何服务——shim 每次调用都会重新读取
```

| 项 | 值 |
|---|---|
| 推理 | OpenRouter Whisper Large V3 Turbo（云端）+ DeepSeek-Chat |
| 4 秒音频延迟 | 约 1-2 秒（转写），AI 问答再加 1-2 秒（LLM）|
| 费用 | $0.04 / 音频小时 + 约 $0.0001 / 次 AI 问答 |
| 服务器占用 | 约 30 KB |
| 服务器 CPU 负载 | 几乎为零 |
| 是否需联网 | 是（HTTPS 访问 openrouter.ai + api.deepseek.com）|

### 本地后端（自部署 whisper.cpp，⚠️ 不推荐）

> ⚠️ **注意**：本地轨道会把 `whisper-small` 模型常驻内存（约
> 1 GB），每次录音都跑一遍 CPU 推理。实测在 2 vCPU 的 AWS
> t3.medium 上、同时跑 VS Code Remote / claude-code / tmux /
> chrome-devtools-mcp 时，转写 4 秒音频要 **约 40 秒**——同一时刻
> 走 OpenRouter 只要 **约 1.5 秒**。API 费用极低（$0.04 / *音频*
> 小时 ≈ 个人每月几美分），除非必须把音频留在本地，否则**强烈建议
> 走云端**。
>
> 如果仍然要装本地轨道，服务器至少需要：
>
> - **4+ 颗空闲 vCPU**（≥3 GHz）——再低，3DS 上的转圈动画会转到
>   让人怀疑人生
> - **2+ GB 空闲内存**给 small.zh 模型 + 缓冲
> - **转写期间没有别的吃 CPU 的进程**（vscode、IDE、playwright 等）
> - **约 600 MB 磁盘**给模型 + venv

确实需要完全离线的话，同一个 `dssh-whisper` CLI 支持双轨共存，
随时切换：

```bash
git clone https://github.com/summerliuguang/3DSSH.git ~/dssh-repo
bash ~/dssh-repo/tools/install_whisper_dual.sh
```

双轨装好后**默认仍走云端**，需要时再切：

```bash
dssh-whisper switch local   # 下次按 START → 本地 whisper.cpp
dssh-whisper switch api     # 下次按 START → OpenRouter（默认）
dssh-whisper switch         # 不带参数 = 来回切换
```

### `dssh-whisper` CLI

```
dssh-whisper status                # 当前轨道 + daemon 状态 + key 是否就位
dssh-whisper switch                # api ↔ local 来回切
dssh-whisper switch [api|local]    # 显式指定
dssh-whisper start                 # 启动本地 daemon（仅双轨安装有效）
dssh-whisper stop | close          # 停本地 daemon
dssh-whisper restart
dssh-whisper logs [-f]             # tail systemd-user 日志
dssh-whisper uninstall             # 全部卸载
```

仅装云端轨道时，daemon 相关命令会自动降级（提示 "no daemon to
start"，不会报错中断）。

### 语音问 AI（L + START）—— v1.1

在 yazi 里忘了显示隐藏文件的快捷键？想要一行 vim 正则？按住
**L** 再按 **START**——这时是在向 AI 提问，不会打进 SSH 会话。
底屏弹出问答窗口，顶屏终端完全不受影响。

| 在弹窗里 | 效果 |
|---|---|
| **A** | 关闭弹窗，**保留**这轮问答；下次 L+START 接着聊 |
| **B** 或触碰下屏 | 关闭弹窗，**清空**历史；下次 L+START 是全新对话 |

对话历史最多保留 5 轮，超出后最旧的一轮被挤掉（FIFO）。

模型是 `deepseek-chat`（DeepSeek 官方 API，不经过 OpenRouter）——
响应快、单价极低（个人用量每问约 $0.0001）。回答通常 6-15 句，
长回答在弹窗内自动换行，超出屏幕的部分以 `...` 截断。

#### 弹窗内的 Markdown 渲染

DeepSeek 的回答常带 Markdown。弹窗会把常见格式渲染成颜色样式，
而不是显示原始符号：

| Markdown 源 | 弹窗渲染 |
|---|---|
| `# 标题` / `## 标题` / `### 标题` | 黄色强调，`#` 号不显示 |
| `` `行内代码` `` | 青色，反引号不显示 |
| ` ``` …代码块… ``` ` | 青色多行，三反引号围栏不显示 |
| `- 项` / `* 项` | `• 项`（灰色圆点 + 普通色内容）|
| `**加粗**` / `*斜体*` | 按普通文字显示——格式符号去掉，无加粗/斜体效果（该字号位图字体没有粗细变体）|
| `[链接文字](url)` | 只显示 `链接文字`，URL 丢弃 |
| `~~删除线~~` | 按普通文字显示，格式符号去掉 |

#### DeepSeek key

需要单独一把 **DeepSeek API key**（在
[platform.deepseek.com/api_keys](https://platform.deepseek.com/api_keys)
申请）；安装脚本会在 OpenRouter key 之后提示这一把（可跳过——纯
语音输入不需要它）。存放在
`~/.config/dssh-whisper/deepseek-key`（chmod 0600），更换方式见上文
[更换 key](#更换-key)。

### 注意事项

- 单次录音上限约 30 秒（1 MB 缓冲在 16 kHz × 16-bit 下约 32 秒，
  软件定时器会提前一点停住）。随时可按 **START** 提前结束。
- 退出 DSSH 请按 **HOME**（不是 START）——START 已专用于语音输入。
- `~/.config/dssh-whisper/` 已在 `.gitignore` 里；换 key 一行命令：
  `echo 'sk-or-v1-NEW' > ~/.config/dssh-whisper/api-key`
- 3DS 端通过 libssh2 exec 调用 `~/.local/bin/dssh-whisper-shim`，
  shim 每次调用时读取当前轨道配置——**切换轨道不需要重启 3DS 或
  SSH 会话**。

---

## 按键说明

### 物理键

| 键 | 功能 | 备注 |
|---|---|---|
| **A** | 回车（英文模式）/ **拼音缓冲区按原样作为英文发送**（中文模式）| 中文模式下误打了英文？按 A 直接把缓冲区里的字母原样发给终端，不用退格、不用切模式重打 |
| **B** | 退格 / 删一个拼音字母 | 按住可自动重复（最快 60 次/秒） |
| **X** | Alt 修饰键 | 按住生效——按住时下一个按键带 Alt |
| **Y** | Ctrl 修饰键 | 按住生效——按住 Y + 点 c → Ctrl-C |
| **L** | Shift 修饰键 / **+ Circle Pad → 滚 tmux 左/上窗格** | 见下文 [滚轮事件的目标位置](#滚轮事件的目标位置tmux-分屏--全屏-tui) |
| **R** | 切换中/英输入模式 | 右上 ENG/CHN 徽章显示当前模式 |
| **SELECT** | Esc | 单击立即发送 |
| **START** | **语音输入开关** | 一按开始录音，再按结束并转写。详见 [语音输入](#语音输入)。 |
| **空格**（软键盘）| 普通空格（英文模式）/ **提交当前高亮候选**（中文模式） | 与搜狗 / fcitx 习惯一致 |
| **Shift + .** | **。**（全角中文句号，U+3002） | 中英文模式都生效 |
| **D-pad ↑↓** | 方向键 / 输入法翻页 | 拼音缓冲区非空时翻候选页 |
| **D-pad ←→** | 方向键 / 输入法选词 | 激活时在当前页内移动候选光标 |
| **Circle Pad ↑↓** | 历史回滚 / 上报 TUI 的鼠标滚轮 | 默认命中**屏幕中央**（opencode / htop / less）；**按住 L → tmux 分屏的左/上窗格** |

> 长按 D-pad 或 B：250ms 后开始重复，0.5s 时 12 次/秒，1.5s 后
> 冲到 60 次/秒。

### 滚轮事件的目标位置（tmux 分屏 / 全屏 TUI）

3DS 没有真正的鼠标光标，鼠标上报型程序（tmux、opencode 等 TUI）
按事件里的 (col, row) 把滚轮路由到对应区域。DSSH 默认发**屏幕中央**
`(40,15)`——全屏 TUI（opencode、htop、less…）的可滚动内容都在
中部，发左上角会被标题栏吞掉、滚不动；按住 **L** 改发 `(1,1)`，
命中 tmux 分屏的**左/上窗格**：

| 操作 | 效果 |
|---|---|
| Circle Pad ↑↓ | 滚动中央区域（全屏 TUI 主内容 / tmux 默认窗格） |
| 按住 L + Circle Pad ↑↓ | 滚动 tmux 分屏的**左/上窗格** |

### 软键盘

下屏整面都是软键盘，共两页：

- **字母页**（默认）：QWERTY 布局，含 `,` `.` 标点、Tab 和加宽
  空格。
- **符号页**（左下 `123` 键切换）：`1234567890` 与 `!@#$%^&*()`
  上下两行对齐，另有 `?`、`\` 等常用符号。

任何键都支持按住修饰键组合，例如**按住 Y + 点 b** = `Ctrl-B`
（tmux 前缀键）。

**SET 按钮**（右下角，v1.2）：打开/关闭机内设置页——服务器列表
`< SRV n/4 >` 切换（浏览到已配置的槽位即选中它）；HOST/PORT/USER/
PASSWORD/KEY PATH 各行点击进编辑（**A** 确认、**B** 退格、
**SELECT** 取消；AUTH 行点击即切换公钥/密码）；VOICE API 直填；
**SAVE** 写回 SD 卡；**RECONNECT** 断开并立即重连。

**WIN 按钮**（SET 左侧，v1.3）：在已配置服务器的窗口间循环切换。
后台窗口保持在线、持续收发数据（fish/tmux 不会卡在未完成的查询
上）；首次切到某个窗口只显示空终端和 "SELECT to connect" 提示，
不会在背后悄悄发起可能卡好几秒的握手——按 **SELECT** 才真正连接。
WIN 标签显示当前窗口号（如 `2/3`），只配置一台服务器时置灰。

### 状态条（顶部 34 px）

```
┌──────────────────────────────────────────────────────┐
│ [SFT]   候选条 / 拼音缓冲区 / 候选词          [CHN]  │
└──────────────────────────────────────────────────────┘
```

- **左槽 [STA]**：3 字母修饰键指示（SFT/CTL/ALT 按住期间常亮；
  ENT/BSP/ESC/`R→C` 在对应事件时闪 200ms）
- **中段**：中文模式显示拼音缓冲区 + 候选词；英文模式为空
- **右槽 [ENG/CHN]**：当前中/英模式；**双击进入 debug 页面**

---

## 输入法用法

### 全拼

中文模式下按字母键，顶部出现候选条：

```
ni       → 年 你 牛奶 娘 念   (page 1/52, total 256)
nihao    → 你好 你好吗 你好啊 拟好 你好呀
shijie   → 世界 世界上 世界杯 世界各地 世界里
```

- **A 键**或**空格**提交当前高亮候选
- **D-pad ←→** 在当前页内移动高亮
- **D-pad ↑↓** 翻页
- **触屏**直接点候选词提交
- **B 键**删掉拼音缓冲区的一个字母

### 缩写（声母）

每个多音节词都有对应的声母条目，只打声母也能召回（权重打 0.3 折，
所以全拼输入时全拼结果仍排最前）：

```
nh → 你好（约第 8 个候选）
wm → 我们（首个）
sj → 世界
zw → 中文
xx → 谢谢
```

翻页或移动光标找到目标后按 A 提交。

### 前缀回退

多打了一个字母？引擎自动取最长有效拼音前缀来匹配，多出的字母
以红色显示：

```
buffer:  nihao[z]    ← 绿色 nihao + 红色 z
候选:    仍显示 nihao 的结果
```

按 B 键删掉红色尾巴即可恢复正常。

### 修饰键始终绕过输入法

中文模式下，按住 **Y + c** 仍然发送 `Ctrl-C`，按住 **L + a** 仍然
发送 `A`。修饰键优先于输入法路由，vim / tmux / claude-code 的
快捷键不受影响。

### 误输英文时：A 键原样发送

中文模式且拼音缓冲区非空时，按 **A** 会把缓冲区里的字母原样发到
SSH 并清空。例如在中文模式下打了 `cd /etc`，候选条会显示一堆奇怪
的中文——这时按一下 **A**，终端收到的就是 `cd /etc`，不用退格、
不用切模式、不用重打。

> 区别：**空格**提交高亮候选（中文上屏）；**A** 把字母原样发出。

---

## Debug 页面

**双击右上角 ENG/CHN 徽章**（500 ms 内点两次）进入 debug 页面，
再单击一次返回。内容包括：

- 标题 + 退出提示。
- **recv hex**：最近 32 字节 SSH 接收数据，用于在字节层面排查
  ANSI / 鼠标协议问题；HTTP 语音的错误响应体也会留在这里。
- **语音后端一行**：`VOICE: HTTP API` 或 `VOICE: SSH shim`。
- **物理键速查表**：上面按键说明的精简版。
- **MASCOT: ON/OFF** 按钮：螃蟹开关（默认 ON）。

---

## 从源码构建

### 依赖

- Linux x86\_64（Ubuntu 22.04 测试通过；其他发行版自行调整包名）。
- [devkitPro / devkitARM](https://devkitpro.org/wiki/Getting_Started)
  release 65+（GCC 14.2 与 16.1 均构建通过）。
- Python 3.10+ 与 Pillow（生成字体和词典）。

### 步骤

```bash
# 1. 安装 devkitPro
wget https://apt.devkitpro.org/install-devkitpro-pacman
bash install-devkitpro-pacman
sudo dkp-pacman -S 3ds-dev 3ds-mbedtls 3ds-libpng 3ds-zlib

# 2. 递归 clone，同时拉取 libts3ds 及其私有 lwIP
git clone --recurse-submodules https://github.com/summerliuguang/3DSSH.git
cd 3DSSH

# 仅已有的非递归 clone 需要执行：
git submodule update --init --recursive

# 3. 交叉编译 libssh2（一次性，产物进 $DEVKITPRO/portlibs/3ds/lib/）
bash build-libssh2.sh

# 4. 安装系统字体（Terminus 提供 ASCII / 制表线字形）
sudo apt install fonts-terminus

# 5. 下载字体源（Zpix）
bash tools/fetch_fonts.sh

# 6. 生成字体 atlas（→ source/font_data.c，约 3 MB）
python3 tools/gen_font.py

# 7. 下载并生成拼音词典（→ romfs/pinyin_dict.bin，约 13 MB）
bash tools/fetch_pinyin_dict.sh
python3 tools/gen_pinyin_dict.py

# 8. 编译 .3dsx（可选：把局域网 STT 端点烘焙成默认语音后端；
#    只在编译期注入，不进仓库）
make DSSH_VOICE_API_DEFAULT=http://your-server:29016/api/stt

# 9. （可选）打包 .cia
bash tools/install_cia_tools.sh   # 装 bannertool + makerom 到 ~/bin
make cia                          # 产出 DSSH.cia
```

构建命令本身：`make` 先构建固定版本的
`libts3ds/build/3ds/libts3ds.a`，再链接 DSSH。如果 submodule 未
初始化，Makefile 会直接打印初始化命令，而不是稍后才报目录或头文件
缺失。

### Host 端测试（无需 3DS）

顶层 Makefile 对所有目标（包括测试）都要求设置 `DEVKITARM`，所以
要么设好环境变量后用 make，要么直接调用测试脚本——它们是纯 host
代码：

```bash
make test-ime test-config test-terminal test-voice-api
# 或者不设 DEVKITARM，直接：
bash tools/test_ime.sh         # IME 引擎冒烟测试
bash tools/test_config.sh      # 配置解析 + 写回回环
bash tools/test_terminal.sh    # 终端协议（滚动、fish 光标）
bash tools/test_voice_api.sh   # HTTP 语音传输的 WAV 封装
```

---

## 项目结构

```
3DSSH/
├── icon.png                   # .3dsx / SMDH 图标（48×48）
├── app.rsf                    # makerom CIA 规格（DSSH+ UniqueId 0xFF55D）
├── Makefile                   # 主构建（make / make cia / make test-*）
├── build-libssh2.sh           # libssh2 + mbedTLS ARM 交叉编译
├── libts3ds/                  # 固定版本的原生 Tailscale 客户端 submodule
├── source/
│   ├── main.c                 # 主循环、SSH 接收、UTF-8 重组
│   ├── ssh_client.{c,h}       # libssh2 封装（含 keychain、Tailscale 接线）
│   ├── config.{c,h}           # SD 卡 config.ini 解析（多服务器）
│   ├── terminal.{c,h}         # ANSI/VT100 解析器（fork 自 skmtrd）
│   ├── renderer.{c,h}         # citro2d 渲染（终端、文本、CJK）
│   ├── keyboard.{c,h}         # 物理按键 + 输入法路由
│   ├── softkb.{c,h}           # 软键盘 + 候选条 + 设置页 + debug 页
│   ├── ime_pinyin.{c,h}       # 拼音引擎
│   ├── voice.{c,h}            # 语音管线：麦克风采集 + SSH shim 传输
│   ├── voice_api.{c,h}        # HTTP 语音传输（WAV 封装 + httpc）
│   ├── ai_modal.{c,h}         # L+START AI 问答弹窗 + Markdown 渲染
│   ├── audio.{c,h}            # 软键盘按键音（DSP，缺失时静默降级）
│   ├── keychain_protocol.h    # macOS 钥匙串解锁引导协议
│   ├── mascot.{c,h}           # 螃蟹吉祥物
│   ├── font_atlas.{c,h}       # 码点 → 字形索引
│   └── font_data.c            # 字体位图（gen_font.py 生成）
├── tools/
│   ├── fetch_fonts.sh         # 下载 Zpix
│   ├── gen_font.py            # 字体 atlas 生成器
│   ├── fetch_pinyin_dict.sh   # 下载 rime-ice
│   ├── gen_pinyin_dict.py     # 词典 → 二进制
│   ├── test_ime.{c,sh}        # host 测试：IME 引擎
│   ├── test_config.{c,sh}     # host 测试：配置解析/写回
│   ├── test_terminal.{c,sh}   # host 测试：终端协议
│   ├── test_voice_api.{c,sh}  # host 测试：WAV 封装
│   ├── install_whisper_api.sh # 服务器端：云端语音安装（shim + CLI）
│   ├── install_whisper_dual.sh# 服务器端：双轨安装（云端 + 本地）
│   ├── whisper_shim.py        # 服务器端：SSH-exec shim（双轨共用）
│   ├── whisper_daemon.py      # 服务器端：本地 whisper.cpp daemon
│   ├── gen_cia_assets.py      # 图标 / banner 派生
│   └── install_cia_tools.sh   # bannertool + makerom 安装
├── romfs/                     # gitignored —— 打包 pinyin_dict.bin
├── data/                      # gitignored —— 字体源 + 词典源
└── sd_template/               # SD 卡部署模板
    ├── README.md
    └── 3ds/3dssh/config.ini.example
```

## 架构概览

```
SSH server (somewhere on the internet)
     ▲ libssh2 over mbedTLS-RSA-4096（或经 libts3ds 走 tailnet）
     │
┌────┴──────────────────────────────────────────────────┐
│  main.c poll loop @60fps                              │
│   ├─ ssh_read（所有窗口）→ utf8 重组                   │
│   │                ↓                                  │
│   │   terminal_write_n → ANSI 解析 → 字符网格          │
│   ├─ hidScanInput → keyboard_handle_input             │
│   │   └─ IME 模式? → ime_input_letter / page / select │
│   ├─ hidTouchRead → softkb_touch                      │
│   │   ├─ 命中候选条 → ime_select                       │
│   │   ├─ 命中按键 → keyboard_emit_for / ime_input      │
│   │   └─ 双击徽章 → debug_mode 切换                    │
│   └─ render: 上屏 = renderer_draw_terminal             │
│              下屏 = softkb_draw + clock + mascot       │
└───────────────────────────────────────────────────────┘
     │
   citro2d (3DS 2D rendering)
     │
   GPU (top 400×240 + bottom 320×240, 24-bit color)
```

详细演进过程见 commit history（M0 → M9 各阶段全部有提交）。

---

## 致谢

### 贡献者

- **[@hedykan](https://github.com/hedykan)** — SELECT 键断线重连、吉祥物状态机、语音打字机流式输出（#5）
- **[@cadl](https://github.com/cadl)** — macOS 钥匙串自动解锁、fish/Kitty 终端协议修复、原生 Tailscale 支持（#6、#7）

### 上游项目

- **[skmtrd/3dssh](https://github.com/skmtrd/3dssh)** — 最早的 3DS
  SSH 客户端（日文版）；本项目的 ANSI/VT100 解析器、UTF-8 重组和
  citro2d 框架都源于它
- **[Fishason/DSSH](https://github.com/Fishason/DSSH)** —— 直接
  上游：拼音输入法、软键盘、云端语音输入
- **[rime-ice](https://github.com/iDvel/rime-ice)** — 拼音词典源
  （固定在 commit `3f57a6f6`）
- **[Zpix Pixel Font](https://github.com/SolidZORO/zpix-pixel-font)** —
  12px CJK 像素字体（OFL 1.1）
- **[Terminus TTF](https://terminus-font.sourceforge.net/)** —
  ASCII / 制表线像素字体
- **[libssh2](https://www.libssh2.org/)** +
  **[mbedTLS](https://www.trustedfirmware.org/projects/mbed-tls/)** —
  SSH / TLS 协议栈
- **[cadl/libts3ds](https://github.com/cadl/libts3ds)** — Nintendo
  3DS 原生 Tailscale 兼容传输，派生自
  **[CamM2325/microlink](https://github.com/CamM2325/microlink)**
- **[devkitPro](https://devkitpro.org/) libctru / citro2d / citro3d** —
  3DS 用户态运行时 + 渲染
- **[carstene1ns/3ds-bannertool](https://github.com/carstene1ns/3ds-bannertool)**
  + **[3DSGuy/Project_CTR makerom](https://github.com/3DSGuy/Project_CTR)** —
  CIA 打包工具

## License

MIT —— 见 [LICENSE](LICENSE)。

字体、词典及上游 SSH/TLS 库各有自己的许可证
（OFL / GPL / BSD / MIT / Apache），分发二进制时请一并遵守。
