# sserver

低延时视频流媒体服务端。C++14 + CMake，支持 V4L2 采集、x264 编码、TCP/UDP/RTP 传输。

## 系统依赖



### 一键全装

```bash
sudo apt update && sudo apt install -y \
  build-essential cmake pkg-config \
  libx264-dev \
  libavcodec-dev libavutil-dev libswscale-dev \
  libopencv-dev \
  ffmpeg v4l-utils
```

## 构建

```bash
cmake -S . -B build
cmake --build build -j
```

或：`./scripts/build.sh`

## 运行

详见 [启动指南](docs/start.md)，包含 TCP/UDP/RTP 各模式的完整用法和常见问题。

## 文档索引

| 文档 | 说明 |
|---|---|
| [启动指南](docs/start.md) | 依赖安装、构建、TCP/UDP/RTP 运行场景、常见问题 |
| [架构总览](docs/src_architecture_overview.md) | 项目整体架构、目录树、数据链路、新人阅读指南 |
| [文档索引](docs/README_INDEX.md) | 所有 README 索引、推荐阅读顺序 |
| [配置文件说明](config/README.md) | 25 个 .conf 文件分类和关键配置项差异 |
| [脚本说明](scripts/README.md) | build/test/tcp/udp/rtp 脚本用法 |
| [测试说明](tests/README.md) | 测试分层、CTest 注册表、运行方式 |
