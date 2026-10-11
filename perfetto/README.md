# Perfetto

Linux 全系统 CPU 调度采集，以及可选的进程内函数、事件埋点。

- `libperflens_trace.a`：默认的静态埋点库；也可以构建 `.so`。
- `perflens-trace`：共享服务管理、检查和采集命令行程序。
- `perflens-demo`：包含多线程计算、休眠、瞬时事件、计数器和 Flow 的示例。

系统调度与应用事件通过同一个 `traced` 服务写入原生 `.perfetto-trace`。
采集范围不按 PID、TID 或 CPU 过滤。函数区间记录总耗时；在 Perfetto UI
中关联线程调度，可以查看区间内在哪些 CPU 上运行、等待调度或阻塞。

## 构建

需要 C++17 编译器、CMake 3.20+、线程库。采集程序在 Linux 上运行；
macOS 可以编译库、示例和运行单元测试，但不能采集 Linux 内核调度。

从项目根目录执行：

```bash
cmake -S perfetto -B perfetto/build -DCMAKE_BUILD_TYPE=Release
cmake --build perfetto/build -j 4
ctest --test-dir perfetto/build --output-on-failure
```

首次配置下载并校验固定版本 **Perfetto v58.2 C++ SDK**。
离线构建可以自行解压官方 `perfetto-cpp-sdk-src.zip`，通过
`-DPERFLENS_PERFETTO_SDK_DIR=/absolute/path/to/sdk` 指定包含
`perfetto.h`、`perfetto.cc` 的目录。版本和 SHA-256 见 [THIRD_PARTY.md](THIRD_PARTY.md)。
也支持在项目根目录用 `cmake -S . -B build` 构建。

共享库构建：

```bash
cmake -S perfetto -B perfetto/build-shared \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON
cmake --build perfetto/build-shared -j 4
```

静态库包含 SDK 对象。共享库只导出 `pl_trace_*` C 接口，隐藏 Perfetto C++
符号。同一个进程的多个动态模块应共同链接一份共享库，避免各自嵌入静态库。

## 安装 Perfetto 运行工具

在 Linux 目标主机执行以下命令，下载经过 SHA-256 校验的官方 `tracebox`：

```bash
python3 perfetto/tools/download_tools.py
```

支持 Linux x86_64、aarch64；默认放到 `perfetto/.deps/bin/tracebox`。
可通过 `--output-dir DIR` 改变位置。CLI 使用 PATH 中的 `tracebox`，
也可用 `--tracebox /absolute/path/to/tracebox` 显式指定。
下载脚本使用 Python 标准库；库和 CLI 在运行时不依赖 Python。

## 全系统采集

需要在 Linux 宿主机执行，并具备访问 tracefs 和 `/proc` 的权限。
普通容器只看到部分进程，无法代表整台宿主机。Linux 虚拟机采集的是
该虚拟机，不能采集外层 macOS/Windows 的调度。

内核应提供 `sched/sched_switch` 和 `sched/sched_waking`。
若 tracefs 尚未挂载，由主机管理员配置，例如：

```bash
sudo mount -t tracefs tracefs /sys/kernel/tracing
```

启动共享服务，再检查环境：

```bash
sudo perfetto/build/perflens-trace service start \
  --tracebox "$PWD/perfetto/.deps/bin/tracebox"

sudo perfetto/build/perflens-trace check \
  --tracebox "$PWD/perfetto/.deps/bin/tracebox"
```

服务默认在后台运行，目录为 `/run/perflens`。`service.log` 记录服务日志。
生产者 socket 的组默认取调用 sudo 的用户的主组，也可通过 `--group GROUP`
指定。组内业务进程可连接生产者 socket，读取 trace 的 consumer socket 和
服务控制 socket 仅供服务所有者访问。业务进程本身不需要 root 权限。

采集所有 CPU 上的进程/线程调度，默认持续 10 秒：

```bash
sudo perfetto/build/perflens-trace record \
  --tracebox "$PWD/perfetto/.deps/bin/tracebox" \
  --duration 10 --output /tmp/system.perfetto-trace
```

输出同时包含：

- `/tmp/system.perfetto-trace`：采集结果，使用 <https://ui.perfetto.dev/> 打开。
- `/tmp/system.perfetto-trace.pbtxt`：此次实际使用的配置，方便复现。

已有输出或配置文件会被拒绝覆盖。Ctrl-C 请求 Perfetto 停止并刷新数据。
采集完成不会停止共享服务。`--duration` 为 1–86400 的整数秒；
`--buffer-mb` 为 1–4096 的中心缓冲区 MiB，默认 64；
`--ftrace-buffer-kb` 为 4–65536 的**每 CPU**内核缓冲区 KiB，默认 2048。
配置每秒写文件，适合持续采集；大量 CPU 和频繁切换需要调整缓冲区。

可先打印配置而不采集：

```bash
sudo perfetto/build/perflens-trace record --duration 10 --dry-run
```

`--dry-run` 仍检查目标主机的 tracefs，只启用实际支持的可选事件。
进程信息通过启动时扫描和 ftrace 新线程触发的按需查询获取。部分可选
线程生命周期、CPU frequency/idle 事件可能因内核或硬件不同而不存在。

## 系统调度与应用埋点联合采集

在业务进程启动前设置 producer socket，并先启动业务程序：

```bash
export PERFETTO_PRODUCER_SOCK_NAME=/run/perflens/producer
perfetto/build/perflens-demo --seconds 15
```

示例最多等待 30 秒，直到采集启用 `demo` category；在另一个终端执行：

```bash
sudo perfetto/build/perflens-trace record \
  --tracebox "$PWD/perfetto/.deps/bin/tracebox" \
  --duration 10 --categories demo \
  --output /tmp/combined.perfetto-trace
```

`--categories` 接受逗号分隔的 category 和通配符，例如
`--categories 'request,network.*'`。只有匹配的应用事件被启用；系统调度
仍覆盖全系统。不传此选项则只采集系统事件。

CLI 会检查是否有 `track_event` producer 接入；实际是否产生了指定 category
的事件，仍应在结果中确认。应用应在采集开始前初始化，并在停止采集前保持运行。
短进程退出前可调用 `pl_trace_flush()` 提交当前线程待写数据。

在 UI 中可查看 `Compute`、`Sleep`、`IterationDone`、`demo/iterations`
以及线程间 Flow，同时在 CPU tracks 查看调度。函数区间长度是总耗时，
包括子调用、等待调度和休眠；on-CPU 时间需要与该线程的 Running 区间取交集。
异步任务应在各个线程分别记录执行片段，再通过 Flow 关联。

## 业务代码接入

同一源码树通过 `add_subdirectory(perfetto)` 引入后：

```cmake
target_link_libraries(my_app PRIVATE perflens::trace)
```

安装后也可使用 CMake 包：

```bash
cmake --install perfetto/build --prefix /your/install/path
```

```cmake
find_package(perflensTrace CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE perflens::trace)
```

配置业务工程时通过 `CMAKE_PREFIX_PATH` 指向安装路径。

```cpp
#include <perflens/trace.hpp>

void HandleRequest() {
  PL_TRACE_SCOPE("request", "HandleRequest");
  // 业务逻辑
  PL_TRACE_INSTANT("request", "ResponseReady");
}

int main() {
  if (!perflens::trace::Initialize()) return 1;
  HandleRequest();
}
```

初始化是进程级、幂等的，应在创建业务线程前调用，socket 环境变量也应在
初始化前设置。返回成功表示 SDK 初始化完成；服务连接与启用采集是异步的。
单次很短的程序需要用 `pl_trace_enabled("request")` 等待采集启用后再执行。

C 调用者使用 `trace.h` 中的 `pl_trace_begin()` / `pl_trace_end()`；
begin 返回 token，传给对应的 end。区间必须在同一 OS 线程上按 LIFO 嵌套。
不支持把同一个 scope 横跨协程挂起后的线程迁移。会话切换后旧 scope 的 end
会被忽略，跨越采集边界的区间可能不完整。封装每个进程同时支持一个采集会话。

字符串支持运行期名称；对外 C 接口内部拦截 C++ 异常。动态 category 的代价
高于原生静态 category，建议使用有限、稳定的 category 名称。当前接口没有
任意参数字典；可通过事件、计数器和进程内 Flow ID 表达基本上下文。

定义 `PERFLENS_TRACE_DISABLED` 可移除 C++ 宏埋点和初始化，宏参数不会求值，
且不需要链接埋点库。直接调用 C 函数不受这个宏影响。
SDK 初始化后禁止在 fork 子进程中继续使用，应 fork 后立即 exec，或在 fork
之前不初始化 SDK。共享库应随业务进程保留到退出，不支持运行中卸载。

## 服务管理

```bash
sudo perfetto/build/perflens-trace service status --tracebox /path/to/tracebox
sudo perfetto/build/perflens-trace service stop
```

`service status` 显示 Perfetto producer 和会话状态。先结束录制，再停止服务。
服务管理只控制本工具创建的实例，使用专属 socket 避免干扰系统已有 Perfetto。
遇到已在使用的 socket 时拒绝启动。也可用 `service start --foreground`
交给外部服务管理器管理，SIGINT/SIGTERM 会清理子进程。

自定义 `--runtime-dir DIR` 时，所有 CLI 命令保持一致，业务进程环境变量也
相应改为 `DIR/producer`。目录应由服务所有者持有，不能由其他用户写入。

## 验证与数据边界

单元测试覆盖配置生成、参数校验、C 接口和编译期禁用。Linux 端到端测试：

```bash
python3 perfetto/tools/download_tools.py --with-analysis --output-dir /tmp/perflens-tools
sudo python3 perfetto/tests/integration.py \
  --build-dir perfetto/build --tools-dir /tmp/perflens-tools
```

测试使用独立服务目录，启动两个带埋点的进程，验证调度数据、进程/线程关联、
函数区间、瞬时事件、计数器、Flow、category 过滤、停止采集和输出保护。
Trace Processor 只用于测试断言，正式采集不依赖它，也没有 SQL 报告模块。

“全系统”指采集范围。权限、PID 命名空间、内核支持、过短的线程生命周期和
缓冲区丢失均会影响完整性。某些极短进程可能仅有 TID 而来不及读取完整名称。
CLI 检查必要事件和数据源、保留错误退出码，但文件非空不代表绝无丢失：
应在 Perfetto UI 的 Info/Stats 检查 data loss，重点关注 ftrace overrun 和
中心缓冲区覆盖。异常中断留下的文件应视为可能不完整。

官方参考：[SDK](https://perfetto.dev/docs/instrumentation/tracing-sdk)、
[调度事件](https://perfetto.dev/docs/data-sources/cpu-scheduling)、
[缓冲区与数据丢失](https://perfetto.dev/docs/concepts/buffers)。
