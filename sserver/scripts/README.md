# scripts

## 1. 目录定位

构建、测试和运行脚本目录。提供一键构建、分层测试、以及各传输后端的快速启动脚本。

## 2. 脚本列表

| 脚本 | 作用 |
|---|---|
| build.sh | 一键构建项目 |
| test.sh | 构建并运行测试，支持按标签分层运行 |
| tcp.sh | 使用 TCP 配置启动服务端（sclient 联调） |
| udp.sh | 使用 UDP 配置启动服务端，支持 --fec / --nack / --profile 选项 |
| rtp.sh | 使用 RTP 配置启动服务端，支持 null / v4l2 两种模式 |

## 3. 脚本详情

### build.sh

一键构建项目。

```bash
./scripts/build.sh
```

等价于：
```bash
cmake -S . -B build
cmake --build build -j
```

### test.sh

构建并运行测试，支持按标签分层。

```bash
./scripts/test.sh              # 运行所有测试
./scripts/test.sh smoke        # 只运行烟雾测试
./scripts/test.sh integration  # 只运行集成测试
./scripts/test.sh benchmark    # 只运行基准测试
```

等价于 `cmake -S . -B build && cmake --build build -j && ctest --test-dir build --output-on-failure -L <label>`。

### tcp.sh

使用 `config/sclient_tcp.conf` 启动服务端（TCP 后端，端口 19099，V4L2 采集）。

```bash
./scripts/tcp.sh
./scripts/tcp.sh --log-level debug
```

适用场景：与 sclient TCP 客户端联调。

### udp.sh

使用 UDP 配置启动服务端，支持多种容错模式。

```bash
./scripts/udp.sh                        # 默认 balanced 预设（裸发）
./scripts/udp.sh --fec                  # 启用 FEC 前向纠错
./scripts/udp.sh --nack                 # 启用 NACK 重传
./scripts/udp.sh --fec --nack           # 全开
./scripts/udp.sh --profile balanced     # balanced 预设（端口 19100，大分片）
./scripts/udp.sh --profile adaptive     # adaptive 预设（端口 19101，小分片，NACK+FEC）
./scripts/udp.sh --profile resilient    # resilient 预设（端口 19102，极小分片，NACK+FEC+重传缓存）
```

预设说明：

| 预设 | 配置文件 | 端口 | NACK | FEC | target_payload | 适用场景 |
|---|---|---|---|---|---|---|
| balanced | sclient_udp.conf | 19100 | 关 | 关 | 65000 | 低延迟局域网 |
| adaptive | sclient_udp_adaptive.conf | 19101 | 开 | 开 | 1000 | 有丢包的网络 |
| resilient | sclient_udp_resilient.conf | 19102 | 开 | 开 | 256 | 高丢包环境 |

`--fec` / `--nack` 选项会基于当前预设配置生成临时配置文件，覆盖对应的 enable 标志，脚本退出时自动清理。

### rtp.sh

使用 RTP 配置启动服务端。

```bash
./scripts/rtp.sh          # null 模式（端口 19504，模拟设备）
./scripts/rtp.sh null     # 同上
./scripts/rtp.sh v4l2     # v4l2 模式（端口 19514，真实摄像头）
```

| 模式 | 配置文件 | 设备 | 远程端口 | SDP 文件 |
|---|---|---|---|---|
| null | integration_rtp_null.conf | null(h264_test_pattern) | 19504 | rtp_null_test.sdp |
| v4l2 | integration_rtp_v4l2.conf | /dev/video0 | 19514 | rtp_v4l2_test.sdp |

RTP 模式下服务端会自动生成 SDP 文件，可配合 ffplay 接收：

```bash
# 启动服务端
./scripts/rtp.sh null &

# 使用 ffplay 接收
ffplay -protocol_whitelist file,udp,rtp -i rtp_null_test.sdp
```

## 4. 通用说明

- 所有脚本都假设项目根目录为脚本所在目录的上级目录
- 所有脚本都使用 `set -euo pipefail` 确保错误时立即退出
- `tcp.sh`、`udp.sh`、`rtp.sh` 都会检查 `build/stream_server` 是否存在，不存在时提示先构建
- 运行脚本前需先执行 `./scripts/build.sh` 或手动构建
