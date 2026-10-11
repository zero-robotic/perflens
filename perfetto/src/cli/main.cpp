#include "config.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace std::chrono_literals;
namespace {
volatile sig_atomic_t interrupted = 0;
void OnSignal(int signal) { interrupted = signal; }
void Signals() {
  struct sigaction action{};
  action.sa_handler = OnSignal;
  sigemptyset(&action.sa_mask);
  sigaction(SIGINT, &action, nullptr);
  sigaction(SIGTERM, &action, nullptr);
  signal(SIGPIPE, SIG_IGN);
}
struct Fd {
  int value = -1;
  explicit Fd(int fd = -1) : value(fd) {}
  ~Fd() { if (value >= 0) close(value); }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
};
[[noreturn]] void Fail(const std::string& message) {
  throw std::runtime_error(message);
}
void SysCheck(bool ok, const std::string& operation) {
  if (!ok) Fail(operation + ": " + std::strerror(errno));
}
void WriteAll(int fd, const std::string& text) {
  size_t written = 0;
  while (written < text.size()) {
    auto n = write(fd, text.data() + written, text.size() - written);
    if (n < 0 && errno == EINTR) continue;
    SysCheck(n > 0, "write");
    written += static_cast<size_t>(n);
  }
}
struct Arguments {
  std::string command, action;
  fs::path runtime = "/run/perflens";
  std::string tracebox = "tracebox", group;
  fs::path output;
  bool dry_run = false, foreground = false;
  perflens::cli::Options config;
};
void Help() {
  std::cout << R"(Usage:
  perflens-trace check [--tracebox PATH] [--runtime-dir DIR]
  perflens-trace service start [--group GROUP] [--foreground] [common options]
  perflens-trace service status|stop [common options]
  perflens-trace record --output FILE [--duration SECONDS] [--categories LIST]
                        [--buffer-mb N] [--ftrace-buffer-kb N] [--dry-run]
                        [common options]

Common options: --tracebox PATH, --runtime-dir DIR (default /run/perflens)
Linux only. Start the shared service before recording. No process/CPU filters.
Recording defaults: 10 seconds, 64 MiB central buffer, 2048 KiB per CPU ftrace
buffer. --categories app,request enables application events as well as scheduling.
Without --categories only system events are collected. Existing files are kept.
--dry-run validates tracefs and prints the configuration without recording.
Business processes must set PERFETTO_PRODUCER_SOCK_NAME=DIR/producer before init.
Applications may connect after recording starts. Wait for "Tracing active" before
launching them to include process and thread creation events.
The service normally runs in the background; logs are stored in DIR/service.log.
Service stop stops only a service managed by this tool; finish recordings first.
)";
}
Arguments Parse(int argc, char** argv) {
  Arguments args;
  if (argc < 2) Fail("missing command; use --help");
  args.command = argv[1];
  int i = 2;
  if (args.command == "service") {
    if (i == argc) Fail("service requires start, status or stop");
    args.action = argv[i++];
    if (args.action != "start" && args.action != "status" && args.action != "stop") Fail("unknown service action");
  } else if (args.command != "check" && args.command != "record") Fail("unknown command: " + args.command);
  std::set<std::string> seen;
  for (; i < argc; ++i) {
    std::string key = argv[i];
    if (!seen.insert(key).second) Fail("duplicate option: " + key);
    bool record_option = key == "--output" || key == "--duration" || key == "--categories" ||
                         key == "--buffer-mb" || key == "--ftrace-buffer-kb" || key == "--dry-run";
    if (record_option && args.command != "record") Fail(key + " is a record option");
    if ((key == "--group" || key == "--foreground") && args.action != "start") Fail(key + " requires service start");
    if (key == "--dry-run") { args.dry_run = true; continue; }
    if (key == "--foreground") { args.foreground = true; continue; }
    if (i + 1 == argc) Fail("missing value for " + key);
    std::string value = argv[++i];
    if (key == "--tracebox") args.tracebox = value;
    else if (key == "--runtime-dir") args.runtime = value;
    else if (key == "--output") args.output = value;
    else if (key == "--group") args.group = value;
    else if (key == "--duration") args.config.duration_ms = perflens::cli::ParseNumber(value, 1, 86400) * 1000;
    else if (key == "--buffer-mb") args.config.buffer_kb = perflens::cli::ParseNumber(value, 1, 4096) * 1024;
    else if (key == "--ftrace-buffer-kb") args.config.ftrace_buffer_kb = perflens::cli::ParseNumber(value, 4, 65536);
    else if (key == "--categories") args.config.categories = perflens::cli::ParseCategories(value);
    else Fail("unknown option: " + key);
  }
  args.runtime = fs::absolute(args.runtime).lexically_normal();
  if ((args.runtime / "consumer").string().size() >= sizeof(sockaddr_un{}.sun_path)) Fail("runtime socket path is too long");
  if (args.command == "record" && args.output.empty() && !args.dry_run) Fail("record requires --output FILE");
  return args;
}
void RequireLinux() {
#ifndef __linux__
  Fail("System scheduling collection requires Linux. Build/run on the Linux target host.");
#endif
}
std::string Executable(const std::string& name) {
  if (name.find('/') != std::string::npos) {
    auto path = fs::absolute(name).string();
    if (access(path.c_str(), X_OK) == 0 && fs::is_regular_file(path)) return path;
  } else {
    std::istringstream paths(std::getenv("PATH") ? std::getenv("PATH") : "");
    std::string dir;
    while (std::getline(paths, dir, ':')) {
      auto path = fs::absolute(fs::path(dir.empty() ? "." : dir) / name).string();
      if (access(path.c_str(), X_OK) == 0 && fs::is_regular_file(path)) return path;
    }
  }
  Fail("tracebox executable not found: " + name + "; see perfetto/tools/download_tools.py");
}
void SocketEnvironment(const fs::path& runtime) {
  SysCheck(setenv("PERFETTO_PRODUCER_SOCK_NAME", (runtime / "producer").c_str(), 1) == 0, "set producer socket");
  SysCheck(setenv("PERFETTO_CONSUMER_SOCK_NAME", (runtime / "consumer").c_str(), 1) == 0, "set consumer socket");
}
pid_t Spawn(const std::vector<std::string>& command, int output = -1) {
  std::vector<char*> argv;
  for (const auto& value : command) argv.push_back(const_cast<char*>(value.c_str()));
  argv.push_back(nullptr);
  pid_t pid = fork();
  SysCheck(pid >= 0, "fork");
  if (!pid) {
    signal(SIGINT, SIG_DFL); signal(SIGTERM, SIG_DFL); signal(SIGPIPE, SIG_DFL);
    // Child gets a separate group; the parent forwards termination once.
    setpgid(0, 0);
    if (output >= 0) { dup2(output, STDOUT_FILENO); dup2(output, STDERR_FILENO); }
    execv(argv[0], argv.data());
    _exit(127);
  }
  return pid;
}
int ExitCode(int status) {
  return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}
void Terminate(pid_t& pid) {
  if (pid <= 0) return;
  kill(pid, SIGTERM);
  for (int i = 0; i < 50; ++i) {
    int status;
    auto result = waitpid(pid, &status, WNOHANG);
    if (result == pid || (result < 0 && errno == ECHILD)) { pid = -1; return; }
    std::this_thread::sleep_for(100ms);
  }
  kill(pid, SIGKILL);
  while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
  pid = -1;
}
struct Result { int code; std::string text; };
Result Run(const std::vector<std::string>& command, int timeout_seconds = 10) {
  char path[] = "/tmp/perflens-query-XXXXXX";
  Fd output(mkstemp(path));
  SysCheck(output.value >= 0, "temporary query file");
  unlink(path);
  fcntl(output.value, F_SETFD, FD_CLOEXEC);
  pid_t pid = Spawn(command, output.value);
  int status = 0;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
  while (true) {
    auto result = waitpid(pid, &status, WNOHANG);
    if (result == pid) break;
    if (result < 0 && errno != EINTR) { Terminate(pid); Fail("waitpid failed"); }
    if (interrupted || std::chrono::steady_clock::now() > deadline) {
      Terminate(pid); Fail("command interrupted or timed out: " + command.front());
    }
    std::this_thread::sleep_for(50ms);
  }
  lseek(output.value, 0, SEEK_SET);
  std::string text;
  char buffer[4096];
  ssize_t count;
  while ((count = read(output.value, buffer, sizeof(buffer))) > 0) {
    text.append(buffer, static_cast<size_t>(count));
    if (text.size() > 4 * 1024 * 1024) Fail("unexpectedly large query output");
  }
  return {ExitCode(status), text};
}
bool Connect(int fd, const fs::path& path) {
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  auto text = path.string();
  if (text.size() >= sizeof(addr.sun_path)) Fail("socket path too long");
  std::memcpy(addr.sun_path, text.c_str(), text.size() + 1);
  return connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
}
bool SocketReady(const fs::path& path) {
  Fd socket_fd(socket(AF_UNIX, SOCK_STREAM, 0));
  return socket_fd.value >= 0 && Connect(socket_fd.value, path);
}
std::set<std::string> Events() {
  if (fs::exists("/.dockerenv") || fs::exists("/run/.containerenv") || fs::exists("/run/systemd/container"))
    std::cerr << "Container environment detected: kernel event PIDs may differ from /proc and SDK PIDs. "
                 "Use the Linux host for complete process attribution.\n";
  for (const auto& root : {fs::path("/sys/kernel/tracing"), fs::path("/sys/kernel/debug/tracing")}) {
    std::ifstream input(root / "available_events");
    if (!input) continue;
    if (access((root / "events/sched/sched_switch/enable").c_str(), W_OK) != 0)
      Fail("cannot access kernel tracing controls; run with sudo on the target host");
    std::set<std::string> events;
    std::string event;
    while (input >> event) {
      std::replace(event.begin(), event.end(), ':', '/');
      events.insert(event);
    }
    if (!events.empty()) return events;
  }
  Fail("tracefs is unavailable. Mount tracefs on the Linux host and grant tracing permissions. "
       "A normal container cannot capture the host's complete process/thread state.");
}
Result Query(const Arguments& args) {
  return Run({args.tracebox, "perfetto", "--query", "--long"});
}
void VerifySources(const Result& result) {
  if (result.code) Fail("cannot query shared traced service:\n" + result.text);
  for (const auto* source : {"linux.ftrace", "linux.process_stats"})
    if (result.text.find(source) == std::string::npos) Fail(std::string("service has no ") + source + " producer");
}
void VerifyDirectory(const fs::path& dir, bool create) {
  if (create && mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) SysCheck(false, "create runtime directory");
  struct stat st{};
  SysCheck(lstat(dir.c_str(), &st) == 0, "inspect runtime directory");
  if (!S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0022))
    Fail("runtime directory must be owned by the current user, not a symlink, and not group/world writable");
}
void RemoveSocket(const fs::path& path) {
  struct stat st{};
  if (lstat(path.c_str(), &st) < 0) { if (errno == ENOENT) return; SysCheck(false, "inspect socket"); }
  if (!S_ISSOCK(st.st_mode) || st.st_uid != geteuid()) Fail("refusing to remove unexpected path: " + path.string());
  SysCheck(unlink(path.c_str()) == 0, "remove stale socket");
}
std::string Group(const Arguments& args) {
  if (!args.group.empty()) {
    if (!getgrnam(args.group.c_str())) Fail("unknown group: " + args.group);
    return args.group;
  }
  gid_t gid = getgid();
  if (geteuid() == 0 && std::getenv("SUDO_GID"))
    gid = perflens::cli::ParseNumber(std::getenv("SUDO_GID"), 0, UINT32_MAX);
  auto* group = getgrgid(gid);
  if (!group) Fail("cannot resolve producer socket group; pass --group");
  return group->gr_name;
}
int Serve(const Arguments& args, int ready_fd, int lock_fd) {
  (void)lock_fd;  // Keep the service lock held for the lifetime of the supervisor.
  pid_t traced = -1, probes = -1;
  bool owns_sockets = false;
  auto cleanup = [&] {
    Terminate(probes); Terminate(traced);
    if (owns_sockets) for (const auto* name : {"control", "producer", "consumer"}) {
      try { RemoveSocket(args.runtime / name); } catch (...) {}
    }
  };
  try {
    Signals();
    auto group = Group(args);
    for (const auto* name : {"producer", "consumer", "control"}) {
      if (SocketReady(args.runtime / name)) Fail("a service is already using " + (args.runtime / name).string());
    }
    for (const auto* name : {"producer", "consumer", "control"}) RemoveSocket(args.runtime / name);
    owns_sockets = true;
    traced = Spawn({args.tracebox, "traced", "--set-socket-permissions", group + ":0660:" + group + ":0600"});
    auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!SocketReady(args.runtime / "consumer")) {
      int status;
      if (waitpid(traced, &status, WNOHANG) == traced) { traced = -1; Fail("traced exited before becoming ready"); }
      if (interrupted || std::chrono::steady_clock::now() > deadline) Fail("traced startup interrupted/timed out");
      std::this_thread::sleep_for(50ms);
    }
    probes = Spawn({args.tracebox, "traced_probes"});
    while (true) {
      auto query = Query(args);
      if (!query.code && query.text.find("linux.ftrace") != std::string::npos &&
          query.text.find("linux.process_stats") != std::string::npos) break;
      int status;
      if (waitpid(probes, &status, WNOHANG) == probes) { probes = -1; Fail("traced_probes exited before becoming ready"); }
      if (interrupted || std::chrono::steady_clock::now() > deadline) Fail("traced_probes startup interrupted/timed out");
      std::this_thread::sleep_for(100ms);
    }
    Fd control(socket(AF_UNIX, SOCK_STREAM, 0));
    SysCheck(control.value >= 0, "control socket");
    fcntl(control.value, F_SETFD, FD_CLOEXEC);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    auto path = (args.runtime / "control").string();
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    SysCheck(bind(control.value, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0, "bind control socket");
    SysCheck(chmod(path.c_str(), 0600) == 0, "control socket permissions");
    SysCheck(listen(control.value, 4) == 0, "listen");
    if (ready_fd >= 0) { WriteAll(ready_fd, "ready\n"); close(ready_fd); ready_fd = -1; }
    std::cout << "Service ready. Producer socket: " << args.runtime / "producer" << std::endl;
    while (!interrupted) {
      for (pid_t* pid : {&traced, &probes}) {
        int status;
        if (waitpid(*pid, &status, WNOHANG) == *pid) { *pid = -1; Fail("Perfetto service child exited unexpectedly"); }
      }
      pollfd event{control.value, POLLIN, 0};
      if (poll(&event, 1, 200) > 0 && (event.revents & POLLIN)) {
        Fd client(accept(control.value, nullptr, nullptr));
        pollfd request{client.value, POLLIN, 0};
        if (client.value >= 0 && poll(&request, 1, 1000) > 0) {
          char command[16]{};
          auto n = read(client.value, command, sizeof(command));
          if (n == 5 && std::string(command, 5) == "stop\n") break;
        }
      }
    }
    cleanup();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << std::endl;
    if (ready_fd >= 0) { try { WriteAll(ready_fd, std::string("error: ") + error.what()); } catch (...) {} close(ready_fd); }
    cleanup();
    return 1;
  }
}
int Start(const Arguments& args) {
  if (geteuid() != 0) Fail("service start requires sudo for full-system tracing");
  auto events = Events();
  perflens::cli::MakeConfig(args.config, events);  // Validate mandatory events.
  VerifyDirectory(args.runtime, true);
  Fd lock(open((args.runtime / "service.lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600));
  SysCheck(lock.value >= 0, "open service lock");
  if (flock(lock.value, LOCK_EX | LOCK_NB) != 0) Fail("a managed service is already running");
  if (args.foreground) return Serve(args, -1, lock.value);
  int pipe_fds[2];
  SysCheck(pipe(pipe_fds) == 0, "startup pipe");
  Fd reader(pipe_fds[0]);
  fcntl(pipe_fds[1], F_SETFD, FD_CLOEXEC);
  pid_t pid = fork();
  SysCheck(pid >= 0, "fork supervisor");
  if (!pid) {
    close(reader.value);
    setsid();
    Fd log(open((args.runtime / "service.log").c_str(), O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0600));
    Fd input(open("/dev/null", O_RDONLY));
    if (log.value < 0 || input.value < 0) {
      auto ignored = write(pipe_fds[1], "log open failed", 15);
      (void)ignored;
      _exit(1);
    }
    dup2(input.value, STDIN_FILENO); dup2(log.value, STDOUT_FILENO); dup2(log.value, STDERR_FILENO);
    _exit(Serve(args, pipe_fds[1], lock.value));
  }
  close(pipe_fds[1]);
  pollfd result{reader.value, POLLIN, 0};
  if (poll(&result, 1, 30000) <= 0) { Terminate(pid); Fail("service startup timed out; inspect service.log"); }
  char buffer[4096]{};
  auto n = read(reader.value, buffer, sizeof(buffer));
  if (n < 6 || std::string(buffer, 6) != "ready\n") {
    Terminate(pid); Fail("service startup failed: " + std::string(buffer, n > 0 ? n : 0));
  }
  std::cout << "Service started.\nApplication environment:\n  export PERFETTO_PRODUCER_SOCK_NAME="
            << (args.runtime / "producer") << "\nLogs: " << (args.runtime / "service.log") << '\n';
  return 0;
}
int Stop(const Arguments& args) {
  VerifyDirectory(args.runtime, false);
  Fd client(socket(AF_UNIX, SOCK_STREAM, 0));
  SysCheck(client.value >= 0 && Connect(client.value, args.runtime / "control"), "connect to managed service control socket");
  WriteAll(client.value, "stop\n");
  auto deadline = std::chrono::steady_clock::now() + 15s;
  while (fs::exists(args.runtime / "control")) {
    if (std::chrono::steady_clock::now() > deadline) Fail("service stop timed out; inspect service.log");
    std::this_thread::sleep_for(100ms);
  }
  std::cout << "Service stopped.\n";
  return 0;
}
int Record(const Arguments& args) {
  auto config = perflens::cli::MakeConfig(args.config, Events());
  if (args.dry_run) { std::cout << config; return 0; }
  VerifySources(Query(args));
  auto output = fs::absolute(args.output);
  if (fs::exists(output) || fs::is_symlink(output)) Fail("output already exists: " + output.string());
  auto config_path = fs::path(output.string() + ".pbtxt");
  Fd config_fd(open(config_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
  SysCheck(config_fd.value >= 0, "create trace configuration");
  WriteAll(config_fd.value, config);
  std::cout << "Recording " << args.config.duration_ms / 1000 << "s; Ctrl-C stops and flushes.\n"
            << "Config: " << config_path << std::endl;
  int ready_pipe[2];
  SysCheck(pipe(ready_pipe) == 0, "recording readiness pipe");
  Fd ready_read(ready_pipe[0]), ready_write(ready_pipe[1]);
  SysCheck(fcntl(ready_read.value, F_SETFD, FD_CLOEXEC) == 0, "readiness pipe flags");
  pid_t pid = Spawn({args.tracebox, "perfetto", "--txt", "-c", config_path.string(),
                     "--no-clobber", "-o", output.string(),
                     "--notify-fd", std::to_string(ready_write.value)});
  close(ready_write.value); ready_write.value = -1;
  int status = 0;
  bool forwarded = false;
  bool ready = false;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(args.config.duration_ms) + 30s;
  while (true) {
    if (!ready && !interrupted) {
      pollfd event{ready_read.value, POLLIN, 0};
      if (poll(&event, 1, 0) > 0) {
        char result = 1;
        if (read(ready_read.value, &result, 1) != 1 || result != 0) {
          Terminate(pid); Fail("Perfetto data sources failed to start; output may be partial");
        }
        ready = true;
        std::cout << "Tracing active. Applications can now be started.\n" << std::flush;
      }
    }
    auto done = waitpid(pid, &status, WNOHANG);
    if (done == pid) break;
    if (done < 0 && errno != EINTR) { Terminate(pid); Fail("recording wait failed"); }
    if (interrupted && !forwarded) {
      kill(pid, SIGINT); forwarded = true;
      deadline = std::chrono::steady_clock::now() + 15s;
    }
    if (std::chrono::steady_clock::now() > deadline) { Terminate(pid); Fail("recording did not finish; output may be partial"); }
    std::this_thread::sleep_for(50ms);
  }
  if (ExitCode(status)) Fail("Perfetto recording failed (exit " + std::to_string(ExitCode(status)) + "); output may be partial");
  if (!fs::exists(output) || fs::file_size(output) == 0) Fail("Perfetto produced no trace data");
  std::cout << "Saved " << output << " (" << fs::file_size(output) << " bytes).\n"
            << "Open in https://ui.perfetto.dev/; check Info and stats for data-loss indicators.\n";
  return 0;
}
}  // namespace
int main(int argc, char** argv) {
  try {
    if (argc == 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) { Help(); return 0; }
    auto args = Parse(argc, argv);
    RequireLinux();
    Signals();
    SocketEnvironment(args.runtime);
    if (args.action == "stop") return Stop(args);
    if (args.command == "record" && args.dry_run) return Record(args);
    args.tracebox = Executable(args.tracebox);
    if (args.command == "check") {
      auto version = Run({args.tracebox, "perfetto", "--version"});
      if (version.code) Fail("cannot execute tracebox: " + version.text);
      std::cout << version.text;
      perflens::cli::MakeConfig(args.config, Events());
      std::cout << "Required scheduling events accessible.\n";
      VerifySources(Query(args));
      std::cout << "Shared service and kernel producers ready.\n";
      return 0;
    }
    if (args.action == "start") return Start(args);
    if (args.action == "status") {
      auto result = Query(args); std::cout << result.text; return result.code;
    }
    return Record(args);
  } catch (const std::exception& error) {
    std::cerr << "perflens-trace: " << error.what() << '\n';
    return 1;
  }
}
