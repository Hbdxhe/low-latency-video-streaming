# 启动指南

## 前置条件

### 安装依赖

```bash
sudo apt update && sudo apt install -y \
  build-essential cmake pkg-config \
  libx264-dev \
  libavcodec-dev libavutil-dev libswscale-dev \
  libopencv-dev \
  ffmpeg v4l-utils
```

### 构建项目

```bash
cd sserver
cmake -S . -B build
cmake --build build -j
```

或使用脚本：

```bash
./scripts/build.sh
```


---

## TCP 模式

**通信模型：** 服务器监听，客户端主动连接

### 服务器端（v4l2，需要摄像头）

```bash
cd sserver
./scripts/tcp.sh
```

- 监听地址：`127.0.0.1:19099`
- 采集源：v4l2
- 编码：x264

### 服务器端（null，无需摄像头）

```bash
cd sserver
./build/stream_server --config config/null_tcp.conf
```

- 监听地址：`127.0.0.1:19099`
- 采集源：null
- 编码：x264

### 客户端（sclient）

```bash
cd sclient
./scripts/tcp.sh
```

---

## UDP 模式

**通信模型：** 服务器监听，客户端主动连接

### 服务器端（v4l2，需要摄像头）

```bash
cd sserver
./scripts/udp.sh
```

默认配置：640x480 30fps，UDP 裸发，一帧一包（65000 字节），无 NACK 无 FEC，最低延迟。

### 服务器端（null，无需摄像头）

```bash
cd sserver
./build/stream_server --config config/null_udp.conf
```

- 监听地址：`127.0.0.1:19100`
- 采集源：null
- 编码：x264

### 可选参数

```bash
./scripts/udp.sh --fec            # 启用 FEC 前向纠错（轻微丢包场景）
./scripts/udp.sh --nack           # 启用 NACK 重传（需要可靠传输）
./scripts/udp.sh --fec --nack     # 全开（网络较差场景）
```

### 预设档位（可选）

```bash
./scripts/udp.sh --profile balanced    # 等同于默认（低延迟）
./scripts/udp.sh --profile adaptive    # FEC 开，适中 payload
./scripts/udp.sh --profile resilient   # NACK + FEC + 小包，高容错
```

| 档位 | payload | NACK | FEC | 适用场景 |
|------|---------|------|-----|----------|
| balanced（默认） | 65000 | off | off | 局域网/本地回环，最低延迟 |
| adaptive | 1000 | off | on | Wi-Fi，有一定抖动 |
| resilient | 256 | on | on | 高丢包网络，偏容错 |

### 客户端

```bash
cd sclient
./scripts/udp.sh
```

---

## RTP 模式

**通信模型：** 服务器主动推送到指定地址（push 模型），客户端被动接收

### 服务器端

```bash
cd sserver
./scripts/rtp.sh null     # null 采集源，推送到 127.0.0.1:19504
./scripts/rtp.sh v4l2     # v4l2 采集源，推送到 127.0.0.1:19514
```

服务器会同时生成 `.sdp` 文件供播放器使用。

### 客户端


```bash
cd sclient
./scripts/rtp.sh null
./scripts/rtp.sh v4l2
```

sclient 不需要 SDP 文件，端口和编码参数直接从配置读取。

### 启动顺序

**推荐：先启动客户端，再启动服务器**

```bash
# 终端 1：先启动客户端监听
cd sclient && ./scripts/rtp.sh null

# 终端 2：再启动服务器推流
cd sserver && ./scripts/rtp.sh null
```

客户端已在监听，服务器推流后立即收到完整画面，无丢帧。

**也可以：先启动服务器，再启动客户端**

```bash
# 终端 1：先启动服务器推流
cd sserver && ./scripts/rtp.sh null

# 终端 2：再启动客户端
cd sclient && ./scripts/rtp.sh null
```

服务器先发出的包客户端收不到（UDP 无连接），日志会出现 `non-existing PPS 0 referenced`，等下一个关键帧（IDR）到来后恢复正常，通常 1-2 秒。

---

## 连接方向对比

| 模式 | 连接方向 | 服务器角色 | 客户端角色 | 典型端口 |
|------|----------|------------|------------|----------|
| TCP | 客户端 → 服务器 | 被动监听 | 主动连接 | 19099 |
| UDP | 客户端 → 服务器 | 被动监听 | 主动连接 | 19100 |
| RTP | 服务器 → 客户端 | 主动推送 | 被动接收 | 19504/19514 |

---

## 常用配置文件

| 配置文件 | 用途 |
|----------|------|
| `config/smoke.conf` | 快速验证（null 采集，无需摄像头） |
| `config/default.conf` | 默认配置（v4l2 + tcp） |
| `config/null_tcp.conf` | TCP null 采集（无需摄像头） |
| `config/null_udp.conf` | UDP null 采集（无需摄像头） |
| `config/sclient_tcp.conf` | TCP 联调（v4l2 采集） |
| `config/sclient_udp.conf` | UDP 联调（默认低延迟，v4l2 采集） |
| `config/integration_tcp.conf` | TCP 集成测试（随机端口） |
| `config/integration_udp.conf` | UDP 集成测试（随机端口） |
| `config/sclient_udp_adaptive.conf` | UDP 联调（FEC 纠错） |
| `config/sclient_udp_resilient.conf` | UDP 联调（NACK + FEC 高容错） |
| `config/integration_rtp_null.conf` | RTP 测试（null 采集） |
| `config/integration_rtp_v4l2.conf` | RTP 测试（v4l2 采集） |

---

## 常见问题

### VIDIOC_S_FMT failed

摄像头被占用。检查并结束占用进程：

```bash
fuser /dev/video0
kill <PID>
```

### non-existing PPS 0 referenced

客户端刚连上时还没拿到关键帧，属于正常现象，后续出图后会消失。
