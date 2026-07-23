# scripts

## 1. 模块定位

启动脚本目录。提供与 `sserver` 联调时的快捷启动脚本，覆盖 TCP、UDP、RTP 三种传输场景。每个脚本封装了常用的命令行参数，减少手动输入。

## 2. 核心职责

- 提供 TCP 联调启动脚本
- 提供 UDP 三档联调启动脚本（balanced/adaptive/resilient）
- 提供 UDP 档位参数配置脚本
- 提供 RTP 联调启动脚本（null/v4l2/sdp）

## 3. 主要文件说明

| 文件 | 作用 |
|---|---|
| `tcp.sh` | TCP 联调启动：固定 host:port + transport tcp + renderer opengl + decoder software |
| `udp.sh` | UDP 联调启动：选择 balanced/adaptive/resilient 档位，委托给 `udp_profile.sh` |
| `udp_profile.sh` | UDP 档位参数配置：根据档位设置不同的 jitter buffer/NACK/FEC 参数 |
| `rtp.sh` | RTP 联调启动：支持 null/v4l2/sdp 三种模式 |

## 4. 脚本详细说明

### tcp.sh

作用：
- 启动 sclient 进行 TCP 联调
- 固定参数：`--host 127.0.0.1 --port 19099 --transport tcp --renderer opengl --decoder software`
- 支持追加额外参数

使用方式：
```bash
./scripts/tcp.sh
./scripts/tcp.sh --vsync on
./scripts/tcp.sh --host 192.168.1.20
```

对应服务端：
```bash
cd sserver && ./scripts/tcp.sh
```

### udp.sh

作用：
- UDP 联调启动入口
- 选择档位后委托给 `udp_profile.sh`
- 默认档位：`balanced`

档位说明：

| 档位 | 端口 | 说明 |
|---|---|---|
| `balanced` | 19100 | 默认档，适合本地回环 / 有线 LAN |
| `adaptive` | 19101 | 有一定抖动时使用，更大的缓冲 |
| `resilient` | 19102 | 更偏容错，最大缓冲 |

使用方式：
```bash
./scripts/udp.sh
./scripts/udp.sh adaptive
./scripts/udp.sh resilient --udp-jitter-buffer off
```

对应服务端：
```bash
cd sserver && ./scripts/udp.sh balanced
cd sserver && ./scripts/udp.sh adaptive
cd sserver && ./scripts/udp.sh resilient
```

### udp_profile.sh

作用：
- UDP 档位参数的实际配置脚本
- 被 `udp.sh` 调用，也可独立使用
- 根据档位设置不同的 jitter buffer 策略、安全系数、最大等待时间、最大帧数等

三档参数对比：

| 参数 | balanced | adaptive | resilient |
|---|---|---|---|
| `--port` | 19100 | 19101 | 19102 |
| `--udp-jitter-buffer-strategy` | 默认(auto) | auto | auto |
| `--udp-jitter-buffer-safety` | 默认(1.5) | 1.5 | 2.5 |
| `--udp-jitter-buffer-max-wait-ms` | 默认(40) | 120 | 250 |
| `--udp-jitter-buffer-max-frames` | 默认(4) | 12 | 32 |
| `--udp-fec` | 默认(on) | on | on |
| `--udp-nack-delay-ms` | 默认(25) | 12 | 12 |

共同参数：`--host 127.0.0.1 --transport udp --renderer opengl --decoder software`

使用方式：
```bash
./scripts/udp_profile.sh balanced
./scripts/udp_profile.sh adaptive --host 192.168.1.20
./scripts/udp_profile.sh resilient --vsync on
```

### rtp.sh

作用：
- RTP 联调启动脚本
- 支持三种模式

模式说明：

| 模式 | 端口 | 说明 |
|---|---|---|
| `null` | 19504 | 监听 127.0.0.1:19504，对应 sserver 的 `config/integration_rtp_null.conf` |
| `v4l2` | 19514 | 监听 127.0.0.1:19514，对应 sserver 的 `config/integration_rtp_v4l2.conf` |
| `sdp <file>` | 从 SDP 读取 | 使用 SDP 文件自动填充 host/port/payload_type/clock_rate |

使用方式：
```bash
./scripts/rtp.sh
./scripts/rtp.sh v4l2
./scripts/rtp.sh sdp ../sserver/rtp_null_test.sdp
./scripts/rtp.sh null --vsync on
```

对应服务端：
```bash
cd sserver && ./scripts/rtp.sh null
cd sserver && ./scripts/rtp.sh v4l2
```

注意：RTP 模式下 `--host` 和 `--port` 表示本地 bind/listen 地址，不是主动连接远端地址。需要先启动 sclient，再让 sserver 往这个地址发流。

## 5. 公共行为

所有脚本共有的行为：
- 检查 `build/sclient` 是否存在，不存在则提示先编译
- 支持 `--help` / `-h` / `help` 显示用法
- 支持追加额外参数（`"$@"`）
- 使用 `exec` 直接替换进程，不产生额外子 shell

## 6. 与其他模块的关系

- 启动 `build/sclient` 可执行文件
- 与 `sserver` 的对应脚本配合使用
- 参数对应 `app/cli/CliOptions.cpp` 中定义的命令行选项

## 7. 使用场景

### 场景 1：TCP 联调

```bash
# 服务端
cd sserver && ./scripts/tcp.sh

# 客户端
cd sclient && ./scripts/tcp.sh
```

### 场景 2：UDP 联调（不同档位）

```bash
# 服务端
cd sserver && ./scripts/udp.sh balanced

# 客户端
cd sclient && ./scripts/udp.sh balanced
```

### 场景 3：RTP 联调

```bash
# 服务端
cd sserver && ./scripts/rtp.sh null

# 客户端
cd sclient && ./scripts/rtp.sh null
```

### 场景 4：使用 SDP 文件

```bash
cd sclient && ./scripts/rtp.sh sdp ../sserver/rtp_null_test.sdp
```

## 8. 调试建议

- 如果提示 `missing client binary`，先执行 `cmake -S . -B build && cmake --build build -j`
- 如果提示 `missing profile script`，确认 `scripts/udp_profile.sh` 存在且可执行
- 可以在脚本参数后追加任意 sclient 参数覆盖默认值
- 使用 `--help` 查看每个脚本的用法

## 9. 后续扩展方向

- 增加 SRT/WebRTC 启动脚本
- 增加录制模式启动脚本
- 增加多实例并行启动脚本
- 增加自动检测 sserver 是否运行的逻辑
