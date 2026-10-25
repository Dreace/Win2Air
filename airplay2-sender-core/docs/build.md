# 构建与测试

从 `airplay2-sender-core/` 执行以下命令。构建需要 CMake/CTest 3.25 以上、Ninja 和 C11 编译器，产物位于忽略的 `out/`。

## Windows

使用原生 MinGW-w64 GCC 13.2.0（Win32 线程、SEH、MSVCRT）。准备静态依赖还需要 Git for Windows 的 Bash、gettext 和 Perl。

```text
cmake -DOUTPUT_DIR=out/windows-toolchain -P platform/windows/prepare-toolchain.cmake
cmake -DBUILD_DIR=<依赖构建目录> -DTOOLCHAIN_DIR=<mingw64目录> -DARCHIVE_DIR=<下载目录> -P platform/windows/build-native-dependencies.cmake
cmake -DBUILD_DIR=<依赖构建目录> -DTOOLCHAIN_DIR=<mingw64目录> -P platform/windows/verify-native-dependencies.cmake
cmake --preset windows-native -DAIRPLAY2_WINDOWS_DEPENDENCIES=<依赖安装目录>
cmake --build --preset windows-native
ctest --preset windows-native --parallel 4 --output-on-failure
```

依赖构建目录使用无空格且不含 `#` 的绝对路径；安装目录包含 `include/`、`lib/` 和 `native-validation.json`。共享 DLL 使用 `windows-app-dll` 预设，同样可指定 `AIRPLAY2_WINDOWS_DEPENDENCIES`。

依赖构建失败后可用 `RESUME=ON` 继续同一次配置，或用 `DEPENDENCIES` 指定重建的库。原始测试日志保留在依赖源码目录。libgpg-error 的测试补丁仅适配 HANDLE 管道及预期路径；FFmpeg 继续执行 HMAC、SHA 和时间解析测试。

Git Bash 中设置 `MSYS_NO_PATHCONV=1`，避免参数路径转换。依赖源码位于其他 Git 仓库目录内时，设置 `GIT_CEILING_DIRECTORIES` 避免补丁被误判为仓库外文件。

## Linux

通过 pkg-config 提供 libevent、libsodium、libgcrypt、libgpg-error、libplist、FFmpeg 4.4 和 uuid 的开发文件。

```text
cmake -S . -B out/linux -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build out/linux
sudo ctest --test-dir out/linux --parallel 4 --output-on-failure --no-tests=error
```

完整配对夹具需要 root 权限以绑定本地 PTP 319/320 端口。执行前退出占用这些端口的发送程序。

## 测试

CTest 直接运行 C 逻辑用例，覆盖会话、回调所有权、配对、PCM、时间戳、延迟参数、音量、RTP 重传、PTP、网络及资源释放。可用 `-L unit`、`-L platform`、`-L contract` 或 `-L integration` 选择范围。

测试仅与本地夹具通信，不依赖参考源码、证据清单或 jq。软件逻辑通过不代表真实接收器已发声，也不测量声学延迟。真实设备验证单独授权。

## Release

使用独立目录执行同一套逻辑测试，Release 也保留测试断言：

```text
cmake --preset windows-app-dll -B out/windows-release -DCMAKE_BUILD_TYPE=Release -DAIRPLAY2_WINDOWS_DEPENDENCIES=<依赖安装目录>
cmake --build out/windows-release
ctest --test-dir out/windows-release --parallel 4 --output-on-failure --no-tests=error
```

Linux 将配置中的 `CMAKE_BUILD_TYPE` 改为 `Release`，使用新的构建目录。发布时保留许可文本及源码版权声明，配对材料和实际设备配置由调用者保管。

调用接口见 [使用指南](usage.md)，版本及模块来源见 [上游来源与同步](upstream.md)。
