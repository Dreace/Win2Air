# 上游来源与同步

上游版本以完整 Git commit ID 标识。许可文本保留在 `LICENSES/`，源码中的版权与许可声明继续保留。

| 仓库 | Commit ID | 用途 |
| --- | --- | --- |
| https://github.com/owntone/owntone-server | `4fa5dccfb69622f42e77bd05628034a69a567d57` | 发送协议、内置 pair_ap、内置 libairptp、RTSP/RTP |
| https://github.com/ejurgensen/pair_ap | `7f53a9c1369162d40c903e3d3df95083ef398d99` | 独立配对实现参考 |
| https://github.com/owntone/libairptp | `7e2252e0258525b3480b54b1906038fee230e981` | 独立 PTP 实现参考 |
| https://github.com/mikebrady/shairport-sync | `01078ad15d4da06dffba0c5f15ffeaf9146a2a2a` | 接收器行为参考 |
| https://github.com/mikebrady/nqptp | `c925f27c1fd12e4033ac477e5a405969b0b0260b` | 接收端时钟参考 |

OwnTone 内置组件与独立仓库分别记录，不能用独立仓库的提交替代内置组件的来源。当前测试直接执行本地逻辑，不依赖上游检出或参考产物目录。

Windows 静态依赖使用 libgpg-error 1.43（LGPL-2.1-or-later）、libgcrypt 1.9.4（LGPL-2.1-or-later）、libsodium 1.0.18（ISC）、libevent 2.1.12（上游 LICENSE）、libplist 2.2.0（LGPL-2.1-or-later）和 FFmpeg 4.4.2（LGPL-2.1-or-later）。各依赖保留独立许可文本。发送器包含 OwnTone 的 GPL 代码，MIT 组件的许可不能替代组合后的许可要求。

## 模块边界

| 独立模块 | 对应上游内容 | 同步时保留的边界 |
| --- | --- | --- |
| `core/session.c` | OwnTone `src/outputs/airplay.c` | RTSP 序列、配对模式、发送及重传、保活；调用者提供目标，运行时排队通知 |
| `core/pairing/` | OwnTone 内置 `src/pair_ap/` | 固定密码后端、普通与瞬态流程；调用者保存长期材料 |
| `core/rtsp.c`、`core/rtp.c` | evrtsp、rtp_common | 消息解析、加密控制、包历史及错误处理 |
| `core/codec.c`、`core/crypto/` | transcode、airplay 密码辅助 | 固定 PCM 格式及 ALAC 封包、原始密码语义 |
| `core/ptpd.c`、`ptp/` | OwnTone 内置 libairptp | 私有 daemon、同步消息及单调时基 |
| `core/runtime.c`、`include/sender.h` | 本地调用边界 | 单活动会话、generation、背压、轮询及锁外回调 |
| `platform/` | 本地系统边界 | socket、时钟、接口、随机、线程与事件；同时保留 POSIX 和 Windows |

库直接链接协议所需的固定依赖。mDNS 发现、数据库、音乐库、Web、播放器、播放列表和其他输出属于原始应用边界，不加入独立库。

## 同步变更

本地扩展 `sender_get_receiver_volume` 通过非阻塞 GET_PARAMETER 查询及认证的 updateInfo 事件提供接收端音量，revision 为零表示尚无有效观察。本地音量成功应用后使旧观察失效。`sender_set_now_playing` 深复制文本与封面，通过 DMAP SET_PARAMETER 和后续图片请求发送；异步结果使用现有 on_command 回调，命令类型为 SENDER_COMMAND_METADATA。控制请求串行执行，背压时由调用者保留最新内容重试。同步上游时保留请求超时恢复、回调上下文释放、空媒体清除及拒绝元数据时不终止音频的边界。

当前协议核心沿用固定 OwnTone 的配对及启动请求顺序、ALAC 分包、RTP 序号与重传、PTP 同步字段和时间戳关系。缓冲与播放提前量可配置、动态参数更新、控制请求确认及串行化属于本地扩展。Windows 的 socket、时钟、线程和私有 PTP 服务属于平台适配。不能将这些扩展描述为与上游全部行为一致。

同步时重点核对 `timestamp_set` 的 `pos + pending - output_buffer_samples`，以及 `packets_sync_send` 中的播放偏移，避免在不同层重复添加预算。保留启动音量放在流配置之后的顺序。配对密码学继续来自固定 pair_ap；配对成功后重新建立验证连接是会话管理策略。

FLUSH 被接收器拒绝时沿用固定 OwnTone 的失败策略，终止失效会话；呈现参数重设失败同样需要重新建立有效会话。本地清空历史不能证明接收器已丢弃旧音频。应用层对带明确丢包标记的短采集缺口补齐对应静音时长，以维持连续采样时间轴；此行为属于发送端采集适配。安全边界和内存所有权修复也可能改变畸形输入的处理，比较时应区分正常协议与异常输入。

同一会话在 FLUSH 后继续递增 RTP 序号，保持与 OwnTone 的包序一致。长时间待机只推进 RTP 时间戳以覆盖时钟间隔，FLUSH 公布的时间戳与恢复音频的起点一致；不能以随机重置包序号代替时间轴同步。同步上游时需保留这一边界，避免接收器将恢复后的包当作旧包或乱序包。

实时采集的 PCM 写入可能不足一个 ALAC/RTP 包。会话只有在首个完整 RTP 包成功发出后才能从已连接转为发送中，起播标记也必须保留到该包；不能根据一次 PCM 写入就提前切换状态。此边界同时适用于初次播放和 FLUSH 后恢复，避免无启动预缓冲时丢失首包标记。

按受影响文件比较对应上游提交，保留本地接口的内存所有权、单会话、取消、重传及恢复边界。更新相关 C 测试后执行 [构建与测试](build.md)，再记录新的上游 commit ID。
