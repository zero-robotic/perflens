#!/usr/bin/env python3
"""Opt-in Linux/root integration test against real tracefs and official tools."""
import argparse
import csv
import grp
import io
import os
import pathlib
import platform
import pwd
import select
import signal
import subprocess
import tempfile
import time


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=pathlib.Path)
    parser.add_argument("--tools-dir", required=True, type=pathlib.Path)
    parser.add_argument("--app-user", help="run demo as this unprivileged user")
    parser.add_argument("--output-dir", type=pathlib.Path, help="keep trace files here")
    args = parser.parse_args()
    require(platform.system() == "Linux" and os.geteuid() == 0, "requires Linux and root")
    cli = str(args.build_dir.resolve() / "perflens-trace")
    demo = str(args.build_dir.resolve() / "perflens-demo")
    tracebox = str(args.tools_dir.resolve() / "tracebox")
    processor = str(args.tools_dir.resolve() / "trace_processor_shell")
    app_user = pwd.getpwnam(args.app_user) if args.app_user else None
    group = grp.getgrgid(app_user.pw_gid if app_user else os.getgid()).gr_name
    with tempfile.TemporaryDirectory(prefix="perflens-it-") as temp:
        root = pathlib.Path(temp)
        root.chmod(0o755)
        runtime = root / "service"
        output_dir = args.output_dir.resolve() if args.output_dir else root
        output_dir.mkdir(parents=True, exist_ok=True)
        common = ["--runtime-dir", str(runtime), "--tracebox", tracebox]

        def run(*command, ok=True):
            result = subprocess.run([cli, *command, *common], text=True, capture_output=True, timeout=40)
            log = runtime / "service.log"
            details = log.read_text(errors="replace")[-6000:] if log.exists() else ""
            require((result.returncode == 0) == ok,
                    f"{command}: exit {result.returncode}\n{result.stdout}\n{result.stderr}\n{details}")
            return result

        def query(trace, sql):
            result = subprocess.run([processor, "query", str(trace), sql],
                                    text=True, capture_output=True, timeout=45, check=True)
            return list(csv.DictReader(io.StringIO(result.stdout)))

        children = []
        recorder = None
        started = False
        try:
            run("service", "start", "--group", group)
            started = True
            run("service", "start", ok=False)
            run("check")
            run("record", "--duration", "-1", "--output", str(root / "invalid"), ok=False)
            # Record before any SDK producer exists, including process/thread
            # creation rather than depending only on the initial /proc scan.
            trace = output_dir / "combined.perfetto-trace"
            recorder = subprocess.Popen([cli, "record", "--duration", "30", "--categories",
                                         "demo", "--output", str(trace), *common], text=True,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            startup = ""
            deadline = time.monotonic() + 15
            while "Tracing active." not in startup:
                readable = select.select([recorder.stdout], [], [],
                                         max(0, deadline - time.monotonic()))[0]
                require(readable, f"recording readiness timed out: {startup}")
                chunk = os.read(recorder.stdout.fileno(), 4096)
                require(chunk, f"recording exited before readiness: {startup}")
                startup += chunk.decode()
            env = dict(os.environ, PERFETTO_PRODUCER_SOCK_NAME=str(runtime / "producer"))
            command = [demo, "--seconds", "5", "--wait-ms", "15000"]
            if app_user:
                command = ["runuser", "-u", app_user.pw_name, "--", *command]
            for _ in range(2):
                children.append(subprocess.Popen(command, env=env, stdout=subprocess.PIPE,
                                                 stderr=subprocess.PIPE, text=True))
            for child in children:
                stdout, stderr = child.communicate(timeout=15)
                require(child.returncode == 0, f"demo failed: {stdout}\n{stderr}")
            recorder.send_signal(signal.SIGINT)
            stdout, stderr = recorder.communicate(timeout=20)
            require(recorder.returncode == 0, f"recording failed: {startup}{stdout}\n{stderr}")
            values = query(trace, """
                SELECT
                  (SELECT count(*) FROM sched WHERE dur > 0) AS scheduling,
                  (SELECT count(DISTINCT t.upid) FROM slice s
                     JOIN thread_track tt ON tt.id=s.track_id
                     JOIN thread t USING (utid)
                     WHERE s.name='Compute' AND s.dur>0) AS apps,
                  (SELECT count(*) FROM slice WHERE name='IterationDone') AS instants,
                  (SELECT count(*) FROM slice WHERE name='MustNotAppear') AS excluded,
                  (SELECT count(*) FROM counter c JOIN counter_track t ON t.id=c.track_id
                     WHERE t.name='demo/iterations') AS counters,
                  (SELECT count(*) FROM flow) AS flows,
                  (SELECT count(*) FROM stats WHERE severity='data_loss' AND value>0) AS loss
            """)[0]
            require(int(values["scheduling"]) > 0, f"missing CPU scheduling: {values}")
            require(int(values["apps"]) == 2, f"two application processes not visible: {values}")
            groups = query(trace, """
                SELECT t.upid,count(DISTINCT t.utid) AS workers FROM slice s
                JOIN thread_track tt ON tt.id=s.track_id JOIN thread t USING(utid)
                WHERE s.name='Compute' AND s.dur>0 GROUP BY t.upid
            """)
            require(len(groups) == 2 and all(g["upid"] != "[NULL]" and int(g["workers"]) == 2
                                            for g in groups),
                    f"workers were not grouped under their application process: {groups}")
            for name in ("instants", "counters", "flows"):
                require(int(values[name]) > 0, f"missing {name}: {values}")
            require(int(values["excluded"]) == 0, "disabled category leaked into trace")
            require(int(values["loss"]) == 0, f"trace has data loss: {values}")
            # Compare CPU and elapsed time on each worker's Sleep spans. This is
            # a test assertion, not a shipped SQL reporting feature.
            sleep = query(trace, """
                WITH spans AS (
                  SELECT s.ts,s.dur,tt.utid FROM slice s
                  JOIN thread_track tt ON tt.id=s.track_id
                  WHERE s.name='Sleep' AND s.dur>0
                )
                SELECT
                  (SELECT sum(dur) FROM spans) AS elapsed,
                  (SELECT sum(min(s.ts+s.dur,r.ts+r.dur)-max(s.ts,r.ts))
                   FROM spans s JOIN sched r ON s.utid=r.utid
                   AND r.dur>0 AND r.ts<s.ts+s.dur AND r.ts+r.dur>s.ts) AS cpu
            """)[0]
            require(sleep["cpu"] != "[NULL]" and sleep["elapsed"] != "[NULL]",
                    "SDK threads did not match scheduler threads. Run on the Linux host in its "
                    "initial PID namespace; container --pid=host can still refer to a nested namespace.")
            require(0 < int(sleep["cpu"]) < int(sleep["elapsed"]) / 2,
                    f"sleep spans did not correlate with CPU scheduling: {sleep}")
            original = trace.read_bytes()
            run("record", "--duration", "1", "--output", str(trace), ok=False)
            require(trace.read_bytes() == original, "existing trace was overwritten")

            interrupted = output_dir / "interrupted.perfetto-trace"
            recorder = subprocess.Popen([cli, "record", "--duration", "30", "--output",
                                         str(interrupted), *common], text=True,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            time.sleep(2)
            recorder.send_signal(signal.SIGINT)
            stdout, stderr = recorder.communicate(timeout=20)
            require(recorder.returncode == 0, f"graceful interrupt failed: {stdout}\n{stderr}")
            require(int(query(interrupted, "SELECT count(*) AS n FROM sched WHERE dur>0")[0]["n"]) > 0,
                    "interrupted capture has no scheduling")
            require(int(query(interrupted, "SELECT count(*) AS n FROM slice WHERE category='demo'")[0]["n"]) == 0,
                    "system-only recording unexpectedly enabled application categories")
            print(f"PASS: system+SDK capture, late producers, grouped workers, category filtering, counters, flows, "
                  f"CPU overlap, no data loss, no clobber, graceful SIGINT.\n{values}\nSleep: {sleep}", flush=True)
            if args.output_dir:
                print(f"Traces: {output_dir}")
        finally:
            if recorder and recorder.poll() is None:
                recorder.terminate()
                recorder.wait(timeout=20)
            for child in children:
                if child.poll() is None:
                    child.terminate()
                    child.wait(timeout=10)
            if started:
                run("service", "stop")
                require(not (runtime / "producer").exists(), "service sockets leaked")


if __name__ == "__main__":
    main()
