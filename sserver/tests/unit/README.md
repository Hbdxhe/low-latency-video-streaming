# tests/unit

## 1. 模块定位

单元测试。验证 `common/`、`config/`、`core/` 层的独立组件逻辑，不依赖完整应用启动。

## 2. 主要文件说明

| 文件 | 测试对象 | 测试内容 |
|---|---|---|
| RtpProtocolTest.cpp | `common/net/RtpProtocol.h` | RTP 头解析/序列化往返、延迟扩展头编解码往返、发送时间戳就地更新 |
| RtpConfigTest.cpp | `config/AppConfig.h` | RTP payload size 边界校验（是否考虑 header overhead）、禁用延迟扩展头 |
| UdpConfigTest.cpp | `config/AppConfig.h` | UDP 配置校验：默认配置合法性、payload size 下限、retransmit cache 参数 |
| UdpProfileConfigTest.cpp | `config/AppConfig.h` | UDP 预设配置文件加载和校验：balanced/adaptive/resilient 三种 profile |
| ConfigLoaderNegativeTest.cpp | `config/AppConfig.h` | 配置加载负测试：缺少等号、未知 key、非法 null_payload_mode、非法 queue_drop_policy、非法 x264 参数 |
| ApplicationLifecycleTest.cpp | `core/Application.h` | Application 模块生命周期：启动失败回滚、初始化失败回滚、成功启动不回滚、首个模块失败无需回滚 |
| LatencyRecorderTest.cpp | `common/metrics/LatencyRecorder.h` | LatencyRecorder 统计准确性：空记录器、单条记录、多条记录、环形缓冲区淘汰、百分位精度 |
| UdpFecRecoveryTest.cpp | `tests/support/TransportTestClient.h` | UDP FEC 恢复：单分片丢失恢复成功、双分片丢失恢复失败 |

## 3. 测试详情

### RtpProtocolTest

- `TestParsePlainRtpHeaderWithoutExtension`：写入不带扩展头的 RTP 包 → 解析 → 校验所有字段
- `TestLatencyExtensionEncodeDecodeRoundTrip`：写入带延迟扩展头的 RTP 包 → 解析 → 校验扩展头字段
- `TestTransportSendTimestampUpdateInPlace`：写入 RTP 包 → 就地更新 send_timestamp_ns → 重新解析 → 校验 capture_timestamp 不变、send_timestamp 已更新

### RtpConfigTest

- `TestAcceptsRtpPayloadThatFitsWithinUdpDatagram`：rtp_max_payload_size = udp_max_datagram_size - overhead，应通过
- `TestRejectsRtpPayloadThatIgnoresHeaderOverhead`：rtp_max_payload_size 超出边界 1 字节，应拒绝
- `TestAllowsDisablingRtpLatencyExtension`：禁用延迟扩展头应通过

### UdpConfigTest

- `TestAcceptsDefaultUdpTransportConfig`：默认 UDP 配置应通过
- `TestRejectsUdpPayloadSizeBelowMinimum`：udp_target_payload_size < 256 应拒绝
- `TestRejectsZeroUdpRetransmitCacheFrames`：retransmit_cache_frames = 0 应拒绝
- `TestRejectsZeroUdpRetransmitMaxFragmentsPerRequest`：retransmit_max_fragments_per_request = 0 应拒绝

### UdpProfileConfigTest

- `TestBalancedProfileMatchesExpectedDefaults`：加载 `config/sclient_udp.conf`，校验端口、payload size、NACK/FEC 配置
- `TestAdaptiveProfileEnablesFecWithLargerJitterTolerance`：加载 `config/sclient_udp_adaptive.conf`，校验 NACK+FEC 启用
- `TestResilientProfileEnablesFecAndRetransmitCache`：加载 `config/sclient_udp_resilient.conf`，校验重传缓存参数

### ConfigLoaderNegativeTest

- `TestRejectsConfigLineWithoutEquals`：缺少 `=` 的配置行应报错
- `TestRejectsUnsupportedConfigKey`：未知配置 key 应报错
- `TestRejectsInvalidNullPayloadMode`：非法 null_payload_mode 应报错
- `TestRejectsInvalidQueueDropPolicy`：非法 queue_drop_policy 应报错
- `TestRejectsInvalidX264IntegerSettings`：x264_threads = 0 应报错

### ApplicationLifecycleTest

- 使用 `RecordingModule`（记录调用顺序）、`FailStartModule`（start 失败）、`FailInitializeModule`（initialize 失败）三个测试替身
- `TestStartFailureRollsBackPreviouslyStartedModules`：moduleC start 失败 → moduleB.stop() → moduleA.stop()（逆序回滚）
- `TestInitializeFailureRollsBackPreviouslyInitializedModules`：moduleC initialize 失败 → moduleB.shutdown() → moduleA.shutdown()
- `TestSuccessfulStartDoesNotTriggerRollback`：成功启动时不应调用 stop()
- `TestFirstModuleFailsStartNoRollbackNeeded`：首个模块失败无需回滚

### LatencyRecorderTest

- 空记录器返回 count=0
- 单条记录正确
- 多条记录的 min/max/avg/p50 正确
- 环形缓冲区淘汰最旧记录
- 恰好 max_samples 条记录后再添加一条
- 100 条记录的 p50/p95/p99 精度

### UdpFecRecoveryTest

- `TestRecoversSingleMissingFragment`：3 个分片丢失 1 个，FEC XOR 恢复成功
- `TestRejectsRecoveryWhenTwoFragmentsAreMissing`：3 个分片丢失 2 个，FEC 恢复失败

## 4. CTest 配置

| 测试名 | 标签 | 超时 |
|---|---|---|
| rtp_protocol_test | unit;rtp | 10s |
| rtp_config_test | unit;rtp | 10s |
| udp_config_test | unit;udp | 10s |
| udp_profile_config_test | unit;udp | 10s |
| config_loader_negative_test | unit;config | 10s |
| application_lifecycle_test | unit;core | 10s |
| latency_recorder_test | unit;metrics | 10s |
| udp_fec_recovery_test | unit;udp;fec | 10s |
