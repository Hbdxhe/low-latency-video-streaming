# sclient

低延时视频接收、解码、渲染客户端。C++14 + CMake，与 `sserver` 配合联调。

- 传输：`tcp` / `udp`（NACK、FEC、自适应抖动缓冲）/ `rtp`
- 解码：FFmpeg 软件解码（H.264）
- 渲染：OpenGL（GLFW + ImGui HUD）
- 观测：端到端延迟、jitter buffer 状态、丢包恢复统计

## 安装依赖

```bash
sudo apt install -y build-essential cmake pkg-config libavcodec-dev libavutil-dev libglfw3-dev libgl-dev
```



## 构建

```bash
cmake -S . -B build
cmake --build build -j
```

## 运行

```bash
./build/sclient --help
```

联调方式见 [sserver 启动指南](../sserver/docs/start.md)。

快速验证（无需 sserver）：

```bash
./build/rtp_decode_smoke
```

## 测试

```bash
ctest --test-dir build --output-on-failure
```

## 文档

- [文档索引](docs/README_INDEX.md) — 架构、模块、测试、按场景推荐阅读路径
