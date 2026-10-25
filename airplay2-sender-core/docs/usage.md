# 使用 Sender 库

公共入口是 `include/sender.h`，CMake 目标为 `airplay2_sender_core`。将源码作为子目录加入调用者工程，然后链接该目标；平台静态库及协议依赖通过 CMake 目标传递。完整协议构建使用 `AIRPLAY2_BUILD_PROTOCOL=ON`，独立调用者可设置 `BUILD_TESTING=OFF`。库不依赖 OwnTone 进程、音乐库、数据库、Web、桌面界面或系统音频采集。

```cmake
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory("实际源码目录/airplay2-sender-core" sender)
add_executable(my_sender main.c)
target_link_libraries(my_sender PRIVATE airplay2_sender_core)
```

原生 Windows 使用固定 MinGW-w64 GCC 13.2.0、MSVCRT 与已通过原测试的静态依赖前缀，配置时传入 `AIRPLAY2_WINDOWS_DEPENDENCIES`。当前 CMake 会直接拒绝其他 Windows 编译器或交叉编译。Linux 依赖由 pkg-config 提供，FFmpeg 使用冻结的 4.4 编码路径。工具链、依赖、Debug/Release 构建和测试命令见 [构建与测试](build.md)，协议模块与版本见 [上游来源与同步](upstream.md)。

## 目标及输入

设备发现由调用者完成。`sender_target` 必须提供数字 IPv4/IPv6 单播地址、非零 RTSP 控制端口、冒号分隔的 MAC 形式接收器身份、非空名称、真实广播 features 和 0～100 的音量。IPv6 链路本地地址必须提供接口 scope；IPv4 的 scope 为零。能力必须包含音频、CoreUtils 配对/加密及 PTP。地址、身份和能力不匹配会在连接前拒绝，不自动选择其他设备或协议模式。

音频为 44100 Hz、16 位、小端、有符号、交错立体声 PCM。`sender_submit()` 的长度单位是字节，必须是完整帧，即四字节的整数倍；库保持格式并编码 ALAC，不自动重采样。创建时 `audio_capacity_bytes` 必须在 1408 字节至 64 MiB 之间且按完整帧对齐。

需要控制播放预算时使用 `sender_create_ex()`，通过 `initial_presentation` 指定基本预算和设备偏移。基本预算及加上偏移后的有效预算均须在 `SENDER_MIN_PRESENTATION_MS` 至 `SENDER_MAX_PRESENTATION_MS` 范围内，当前为 20–5000 ms。`buffers` 分别设置启动预填、render、安全余量和重传历史；启动预填、render 与安全余量之和须适配有效预算。活动会话通过 `sender_set_presentation()` 提交调整，完成回执成功后使用新值。低预算属于发送配置，实际接收器可以拒绝，配置数值不代表声学实测。

## 事件驱动及生命周期


SETUP 日志中的 `available` 和 `invalid` 按位对应 `latencyMin`、`latencyMax`、`arrivalToRenderLatencyMs`。缺失与接收端报告零分别记录，前两项保留采样帧单位，后一项保留毫秒单位。这些响应值不代表播放完整性；空的反馈流列表也不作为播放失败判据。诊断本身不改变播放预算。

`sender_get_timing()` 的 `receiver_render_available` 和 `receiver_render_ms` 提供当前流 SETUP 响应中的 `arrivalToRenderLatencyMs`。调用者先检查可用标记，再读取毫秒值；缺失不代表零，重新连接会清除旧流观察。开始提交音频前可以通过 `sender_retarget_presentation()` 应用由响应计算的目标。已有有声音频时调低目标仍需通过 `sender_set_presentation()` 清流并更新锚点，避免覆盖尚未播放的片段。

创建时设置稳定且非零的 `sender_id`、非空 `client_name` 和 `user_agent`、容量、状态回调及回调上下文。实机驱动使用 `AirPlay/999.0.0` 请求标识，避免已观察到的设备信息请求拒绝。回调参数在回调期间借用；应用需要长期使用时自行复制。

调用 `sender_connect()`，保留成功返回的 generation，并从同一个执行上下文持续调用 `sender_poll()` 驱动排队操作和网络事件。连接返回成功表示请求已接纳，实际就绪由 `sender_get_state()` 或 `on_state` 通知判定。到达 `SENDER_READY` 或 `SENDER_STREAMING` 后提交 PCM；调用者按样本时长控制节奏，轮询本身不替调用者生成音频或控制输入节奏。

`sender_submit()` 返回 `SENDER_OK` 时库已复制并接受全部输入；其他结果接受零字节。`SENDER_BACKPRESSURE` 表示当前队列容量不足，应继续 poll，容量释放后重试同一完整输入；超过配置容量的单次输入直接拒绝。公共接口没有独立可写回调，应用通过结果及事件驱动控制重试。

`sender_flush()` 排队清空，清除待发送音频和旧重传历史；已经发送的在途包仍可能到达。等待状态回到可发送后再提交新音频。`sender_end()` 排队结束并清理，重复结束在进行中保持幂等，清理后返回 `SENDER_ALREADY_ENDED`。失败路径也释放会话资源，终态通知保留具体原因。库不自动重连；终态回调结束且当前 poll 返回后，调用者显式再次 connect。

进程内所有运行时共同限制为一个活动发送会话，占用从连接开始持续至资源释放完成。每个操作使用当前 generation，旧 generation 被拒绝。回调可提交音频、FLUSH 或结束，这些操作进入队列；回调内不能递归 poll、建立新连接或销毁运行时。销毁前等待终态和 poll 返回，再调用 `sender_destroy()`。

## 配对及调用者保存材料

初次普通配对由调用者提供密码；`on_credentials` 在成功后回传接收器身份、发送器身份及配对材料。调用者在回调返回前复制材料，负责安全保存、替换和显式删除，并自行处理存储失败。库复制连接输入用于当前会话，结束清除内部副本，始终不写入文件或凭据服务。

再次或跨进程连接时，调用者提供相同稳定发送器身份及对应 `sender_credentials`，无需重复提供密码；身份不匹配在联网前拒绝。认证失败由回调明确通知，库不自动删除调用者材料或重试。瞬态配对只有当前会话材料，不能用来证明普通配对的长期凭据复用。真实材料不能写入公开日志、示例配置或测试报告。

普通配对、跨进程材料复用及加密控制由本地 pair_ap server 夹具检查。逻辑用例的通过结果不替代真实接收器的播放验证。

## 诊断与验收

即时返回结果和异步回调的 `sender_error` 共同构成错误结果。保留 `operation`、`stage`、`generation`、`platform_code`、`peer_code` 与安全 `message`，不要仅记录错误枚举。正常终态不应覆盖之前收到的错误；最终验收驱动遇到非成功错误回调会拒绝通过，即使本地清理已完成。

逻辑测试范围和执行方式见 [构建与测试](build.md)。连接成功、发送计数或时钟状态不能单独证明接收端已发声；实际设备播放效果需要独立验证。
