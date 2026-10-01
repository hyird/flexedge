#!/usr/bin/env python3
"""Drive the dedicated loopback Ruvia fixture against an isolated PostgreSQL QA DB."""

import concurrent.futures
import os
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request


class ProbeFailure(RuntimeError):
    pass


def require(condition, message):
    if not condition:
        raise ProbeFailure(message)


def request(base, path, timeout=8):
    try:
        with urllib.request.urlopen(base + path, timeout=timeout) as response:
            return response.status, response.read().decode("utf-8")
    except urllib.error.HTTPError as error:
        return error.code, error.read().decode("utf-8")


def start_fixture(binary, port):
    process = subprocess.Popen(
        [binary, str(port)],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        env=os.environ.copy(),
    )
    base = f"http://127.0.0.1:{port}"
    for _ in range(100):  # Fixture-startup readiness only; never used for business polling.
        if process.poll() is not None:
            raise ProbeFailure(f"fixture exited during startup with {process.returncode}")
        try:
            status, body = request(base, "/ready", timeout=0.2)
            if status == 200 and body == "ready":
                return process, base
        except (OSError, TimeoutError):
            pass
        time.sleep(0.05)
    process.terminate()
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
    raise ProbeFailure("fixture did not become ready")


def free_loopback_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def check_response(base, path, expected):
    status, body = request(base, path)
    require(status == 200, f"{path}: HTTP {status}: {body}")
    require(body == expected, f"{path}: expected {expected!r}, got {body!r}")


def wait_for_follower_state(base, predicate, timeout=3):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        status, body = request(base, "/short-follower-state")
        require(status == 200, f"short follower state: HTTP {status}: {body}")
        state = tuple(map(int, body.split(",")))
        if predicate(state):
            return state
        time.sleep(0.01)
    raise ProbeFailure(f"short follower fixture did not reach expected stage; last state={state!r}")


def main():
    if len(sys.argv) != 2:
        raise ProbeFailure("usage: snapshot_read_budget_test.py FIXTURE_BINARY")
    required = ("PGHOST", "PGDATABASE", "PGUSER", "PGPASSWORD")
    if any(not os.environ.get(name) for name in required):
        print("SKIP: isolated PostgreSQL environment is not configured")
        return 77
    host = os.environ["PGHOST"]
    database = os.environ["PGDATABASE"]
    if host != "127.0.0.1" or not database.endswith("_auth_qa"):
        raise ProbeFailure("refusing anything except loopback PostgreSQL *_auth_qa")
    if not os.environ.get("PGPORT"):
        os.environ["PGPORT"] = "5432"

    print("PostgreSQL fixture: " + " ".join(
        f"{key}={os.environ.get(key, '')}" for key in ("PGHOST", "PGPORT", "PGDATABASE", "PGUSER")
    ))
    binary = os.path.abspath(sys.argv[1])
    port = free_loopback_port()
    process, base = start_fixture(binary, port)
    try:
        check_response(base, "/owner", "owner-ok")
        check_response(base, "/business-error", "business-error-cached")

        short_started = time.monotonic()
        check_response(base, "/short", "short-budget-cancelled")
        short_elapsed = time.monotonic() - short_started
        require(0.20 <= short_elapsed < 2.0, f"short budget elapsed {short_elapsed:.3f}s")
        check_response(base, "/health-db", "db-healthy")

        late_started = time.monotonic()
        check_response(base, "/late-result", "late-result-discarded-reclaimed")
        late_elapsed = time.monotonic() - late_started
        require(0.70 <= late_elapsed < 2.5, f"late fetch completion elapsed {late_elapsed:.3f}s")

        check_response(base, "/context-stop", "context-stop-cancelled")
        check_response(base, "/health-db", "db-healthy")
        check_response(base, "/ticket-generation", "ticket-generation-ok")

        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
            short_leader = executor.submit(request, base, "/short-follower?mode=leader", 4)
            leader_stage = wait_for_follower_state(base, lambda state: state[0] == 1 and state[1] == 0)
            follower_submitted = time.monotonic()
            short_follower = executor.submit(request, base, "/short-follower?mode=follower", 4)
            follower_stage = wait_for_follower_state(base, lambda state: state[0] == 1 and state[1] == 1)
            follower_wait_before_reclaim_ms = follower_stage[3]
            require(follower_stage[2] == 0,
                    f"follower reached its query before joining the active leader: {follower_stage!r}")
            require(0 <= follower_wait_before_reclaim_ms < 250,
                    f"follower did not join early in the leader flight: {follower_stage!r}")
            short_leader_status, short_leader_body = short_leader.result(timeout=3)
            short_follower_status, short_follower_body = short_follower.result(timeout=3)
            short_follower_elapsed = time.monotonic() - follower_submitted
        require(short_leader_status == 200 and short_leader_body == "short-leader-cancelled",
                f"short leader was not cancelled: HTTP {short_leader_status}, {short_leader_body!r}")
        require(short_follower_status == 200 and short_follower_body == "short-follower-deadline",
                f"follower budget reset while reclaiming: HTTP {short_follower_status}, {short_follower_body!r}")
        require(1.10 <= short_follower_elapsed < 1.55,
                f"follower did not time out on its original deadline: {short_follower_elapsed:.3f}s")
        check_response(base, "/short-follower-metrics", "1,0")
        check_response(base, "/health-db", "db-healthy")

        # The leader runs two sequential pg_sleep(18) statements on one scoped
        # DbHandle. Its follower shares the lease and can only proceed after the
        # cancelled flight is released and reclaimed.
        baseline_status, baseline_metrics = request(base, "/metrics")
        require(baseline_status == 200, "could not read pre-aggregate metrics")
        baseline_queries, baseline_flights = map(int, baseline_metrics.split(","))
        require(baseline_flights == 0, f"unexpected pre-existing flight count {baseline_flights}")
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
            aggregate_started = time.monotonic()
            leader = executor.submit(request, base, "/aggregate?key=reclaim", 70)
            time.sleep(0.4)
            follower = executor.submit(
                request, base, "/aggregate?key=reclaim&mode=fast", 70
            )
            follower_status, follower_body = follower.result(timeout=65)
            leader_status, leader_body = leader.result(timeout=10)
            aggregate_elapsed = time.monotonic() - aggregate_started
        require(leader_status == 200 and leader_body == "cancelled-or-deadline",
                f"aggregate leader result: HTTP {leader_status}, {leader_body!r}")
        require(29.5 <= aggregate_elapsed < 33.0,
                f"aggregate deadline elapsed {aggregate_elapsed:.3f}s")
        require(follower_status == 200 and follower_body == "follower-reclaimed",
                f"follower did not reclaim cancelled flight: HTTP {follower_status}, {follower_body!r}")
        metrics_status, metrics_body = request(base, "/metrics")
        require(metrics_status == 200, "could not read post-aggregate metrics")
        aggregate_queries, aggregate_flights = map(int, metrics_body.split(","))
        require(aggregate_queries - baseline_queries == 3 and aggregate_flights == 0,
                f"expected two leader queries, one follower retry and no flight; before={baseline_metrics!r}, after={metrics_body!r}")
        print(f"aggregate_elapsed={aggregate_elapsed:.3f}s; leader=2 pg_sleep(18) on one handle; "
              f"follower=reclaimed; metrics_before={baseline_metrics}; metrics_after={metrics_body}")
        check_response(base, "/health-db", "db-healthy")

        # Stop a worker while a real PostgreSQL statement is in flight. The
        # fixture is restarted afterward and the new pool must answer a query.
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as executor:
            in_flight = executor.submit(request, base, "/worker-stop", 30)
            time.sleep(0.25)
            check_response(base, "/worker-started", "started")
            process.terminate()
            process_status = process.wait(timeout=8)
            require(process_status == 0,
                    f"server did not shut down normally after worker stop: status={process_status}")
            try:
                in_flight.result(timeout=2)
            except (OSError, TimeoutError, concurrent.futures.CancelledError):
                pass
        process, base = start_fixture(binary, free_loopback_port())
        check_response(base, "/health-db", "db-healthy")
        print(f"short follower: waiting={follower_wait_before_reclaim_ms}ms; "
              "remaining budget at reclaim <= 300ms + wait < 550ms < pg_sleep(650ms) "
              "< reset budget(1300ms); elapsed="
              f"{short_follower_elapsed:.3f}s; same-budget timeout confirmed")
        print("short budgets, late-result non-publication, A/B ticket generation, follower deadline, "
              "context/worker cancellation, business failure, owner borrow, and pool recovery passed")
        return 0
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ProbeFailure, subprocess.TimeoutExpired, OSError) as error:
        print(f"snapshot read budget probe failed: {error}", file=sys.stderr)
        sys.exit(1)
