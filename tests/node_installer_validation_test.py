"""Exercise installer validation and cleanup without network or system writes."""

import os
import hashlib
import fcntl
from pathlib import Path
import subprocess
import sys
import tempfile


def run_installer(script, env, node_id=b"a" * 32, secret=b"!" * 32):
    return subprocess.run(
        ["/bin/bash", os.fsencode(script), node_id, secret], env=env,
        capture_output=True, timeout=5,
    )


def main():
    installer = os.fsencode(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="flexedge-installer-validation-") as root:
        for name, body in {
            "id": "#!/bin/sh\necho 0\n",
            "mktemp": "#!/bin/sh\nexit 77\n",
        }.items():
            path = Path(root) / name
            path.write_text(body)
            path.chmod(0o700)
        env = dict(os.environ, PATH=root + ":/usr/bin:/bin",
                   FLEXEDGE_SERVER_ORIGIN="https://example.invalid", LC_ALL="C.UTF-8")
        count = 0

        def check(node_id, secret, valid):
            nonlocal count
            result = run_installer(installer, env, node_id, secret)
            expected = 77 if valid else 1
            if result.returncode != expected:
                raise AssertionError(
                    f"case {count}: expected exit {expected}, got {result.returncode}"
                )
            count += 1

        # NUL cannot be passed in a process argument.
        for byte in range(1, 256):
            check(b"a" * 32, bytes([byte]) * 32, 0x21 <= byte <= 0x7e)
        for size in (0, 31, 32, 128, 129):
            check(b"a" * 32, b"!" * size, 32 <= size <= 128)
        for node_id in (b"", b"a" * 31, b"a" * 33, b"A" * 32, b"g" * 32):
            check(node_id, b"!" * 32, False)
        check(b"0123456789abcdef" * 2, b"~" * 128, True)
        for origin, valid in (
            ("https://example.invalid", True),
            ("http://127.0.0.1:8080/", True),
            ("https://[::1]:443", True),
            ("https://example.invalid:00080", True),
            ("https://", False),
            ("https://example.invalid/path", False),
            ("https://example.invalid\nRestart=no", False),
            ("https://example.invalid argument", False),
            ("https://user@example.invalid", False),
            ("https://example.invalid?x", False),
            ("https://example.invalid#x", False),
            ("https://example.invalid/%i", False),
            ("https://$HOST", False),
            ("https://example.invalid:", False),
            ("https://example.invalid:0", False),
            ("https://example.invalid:65536", False),
        ):
            env["FLEXEDGE_SERVER_ORIGIN"] = origin
            check(b"a" * 32, b"!" * 32, valid)
        print(f"{count} installer validation cases passed")

        # Exercise the real EXIT trap after credentials have been written.
        # The network command fails before installation touches the system.
        (Path(root) / "mktemp").write_text(
            '#!/bin/sh\nexec /usr/bin/mktemp -d "$TEST_ROOT/staging.XXXXXX"\n'
        )
        curl = Path(root) / "curl"
        curl.write_text("#!/bin/sh\nexit 77\n")
        curl.chmod(0o700)
        env.update(TEST_ROOT=root, FLEXEDGE_SERVER_ORIGIN="https://example.invalid")
        check(b"a" * 32, b"!" * 32, True)
        if list(Path(root).glob("staging.*")):
            raise AssertionError("installer left staging files after download failure")
        print("installer download failure cleanup passed")

        # Stop even a valid download at the first installation command.
        install = Path(root) / "install"
        install.write_text("#!/bin/sh\nexit 78\n")
        install.chmod(0o700)
        curl.write_text('''#!/bin/sh
while [ "$#" -gt 0 ]; do
    case "$1" in
        --dump-header) shift; headers="$1" ;;
        --output) shift; binary="$1" ;;
    esac
    shift
done
printf 'test binary' > "$binary"
printf 'HTTP/1.1 200 OK\\r\\nX-FlexEdge-Node-SHA256: %s\\r\\nX-FlexEdge-Node-Version: %s\\r\\n\\r\\n' "$TEST_DIGEST" "$TEST_VERSION" > "$headers"
''')
        digest = hashlib.sha256(b"test binary").hexdigest()
        for checksum, version, expected in (
            (digest, "1.2.3", 78),
            ("", "1.2.3", 1),
            ("a" * 63, "1.2.3", 1),
            ("g" * 64, "1.2.3", 1),
            ("0" * 64, "1.2.3", 1),
            (digest, "", 1),
            (digest, "bad/version", 1),
            (digest, "a" * 65, 1),
            (digest, "1.2.3 extra", 1),
            (digest + " extra", "1.2.3", 1),
            (" \t" + digest + "\t ", " \t1.2.3\t ", 78),
        ):
            env.update(TEST_DIGEST=checksum, TEST_VERSION=version)
            result = run_installer(installer, env)
            if result.returncode != expected:
                raise AssertionError(
                    f"artifact gate: expected {expected}, got {result.returncode}"
                )
            if list(Path(root).glob("staging.*")):
                raise AssertionError("artifact gate left staging files")
        print("installer artifact validation and cleanup passed")

        target = Path(root) / "installed"
        target.mkdir()
        (target / "node").write_text("old binary")
        (target / "credentials").write_text("old credentials")
        (target / "state").mkdir()
        (target / "state" / "sentinel").write_text("old state")
        isolated = Path(root) / "isolated-installer.sh"
        isolated.write_text(Path(os.fsdecode(installer)).read_text().replace(
            "/opt/flexedge", str(target)
        ).replace("/etc/systemd/system/flexedge-node.service", str(Path(root) / "unit")))
        install.write_text('#!/bin/sh\nexec /usr/bin/install "$@"\n')
        systemctl = Path(root) / "systemctl"
        systemctl.write_text('''#!/bin/sh
case "$1" in
    show) echo loaded ;;
    stop) exit 43 ;;
    *) exit 44 ;;
esac
''')
        systemctl.chmod(0o700)
        env.update(TEST_DIGEST=digest, TEST_VERSION="1.2.3")
        result = run_installer(isolated, env)
        if result.returncode != 43:
            raise AssertionError(f"stop failure: unexpected exit {result.returncode}")
        for name, expected in (("node", "old binary"), ("credentials", "old credentials"),
                               ("state/sentinel", "old state")):
            if (target / name).read_text() != expected:
                raise AssertionError(f"stop failure modified {name}")
        if list(Path(root).glob("staging.*")):
            raise AssertionError("stop failure left staging files")
        print("installer stop failure preserves existing resources")
        with (target / ".install.lock").open("w") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            result = run_installer(isolated, env)
            if result.returncode != 1:
                raise AssertionError(f"installation lock: unexpected exit {result.returncode}")
        if list(Path(root).glob("staging.*")):
            raise AssertionError("lock rejection left staging files")
        print("concurrent installer rejected")
        unit = Path(root) / "unit"
        unit.write_text("old unit")
        install.write_text('''#!/bin/sh
for argument do destination="$argument"; done
if [ "$TEST_FAILURE" = write ] && [ "$destination" = "$TEST_TARGET/credentials" ]; then
    exit 46
fi
exec /usr/bin/install "$@"
''')
        systemctl.write_text('''#!/bin/sh
echo "$1" >> "$TEST_ROOT/service-calls"
case "$1" in
    show) echo loaded ;;
    restart) exit 45 ;;
    *) exit 0 ;;
esac
''')
        for failure, expected_exit in (("write", 46), ("restart", 45)):
            calls = Path(root) / "service-calls"
            calls.write_text("")
            env.update(TEST_FAILURE=failure, TEST_TARGET=str(target))
            result = run_installer(isolated, env)
            if result.returncode != expected_exit:
                raise AssertionError(f"{failure} rollback: {result.returncode}, {result.stderr!r}")
            for name, expected in (("node", "old binary"), ("credentials", "old credentials"),
                                   ("state/sentinel", "old state")):
                if (target / name).read_text() != expected:
                    raise AssertionError(f"{failure} rollback did not restore {name}")
            if unit.read_text() != "old unit" or calls.read_text().splitlines()[-1] != "start":
                raise AssertionError(f"{failure} rollback did not restore old service")
            if list(Path(root).glob("staging.*")):
                raise AssertionError(f"{failure} rollback left staging files")
        print("installer write and restart failure rollback passed")
        # Fail after one snapshot was copied, before any installed file changes.
        copy = Path(root) / "cp"
        copy.write_text('''#!/bin/sh
for argument do destination="$argument"; done
case "$destination" in
    */backup/1) exit 47 ;;
esac
exec /usr/bin/cp "$@"
''')
        copy.chmod(0o700)
        calls.write_text("")
        env.update(TEST_FAILURE="backup")
        result = run_installer(isolated, env)
        if result.returncode != 47:
            raise AssertionError(f"partial backup failure: {result.returncode}, {result.stderr!r}")
        for name, expected in (("node", "old binary"), ("credentials", "old credentials"),
                               ("state/sentinel", "old state")):
            if (target / name).read_text() != expected:
                raise AssertionError(f"partial backup failure modified {name}")
        if unit.read_text() != "old unit":
            raise AssertionError("partial backup failure modified service unit")
        actions = calls.read_text().splitlines()
        if actions[-2:] != ["stop", "start"] or "daemon-reload" in actions:
            raise AssertionError(f"partial backup failure service recovery: {actions}")
        if list(Path(root).glob("staging.*")):
            raise AssertionError("partial backup failure left staging files")
        copy.unlink()
        print("partial backup failure preserves files and restarts old service")
        fresh = Path(root) / "fresh"
        fresh_unit = Path(root) / "fresh-unit"
        isolated.write_text(isolated.read_text().replace(str(target), str(fresh)).replace(
            str(unit), str(fresh_unit)
        ))
        systemctl.write_text('''#!/bin/sh
case "$1" in
    show) echo not-found ;;
    is-active|is-enabled) exit 1 ;;
    stop) exit 5 ;;
    daemon-reload) exit 0 ;;
    *) exit 44 ;;
esac
''')
        env.update(TEST_FAILURE="write", TEST_TARGET=str(fresh))
        result = run_installer(isolated, env)
        if result.returncode != 46:
            raise AssertionError(f"fresh installation rollback: {result.returncode}")
        if any((fresh / name).exists() for name in ("node", "credentials", "state")):
            raise AssertionError("fresh installation rollback left installed resources")
        if fresh_unit.exists() or list(Path(root).glob("staging.*")):
            raise AssertionError("fresh installation rollback left unit or staging")
        print("fresh installation write failure rollback passed")
        isolated.write_text(Path(os.fsdecode(installer)).read_text().replace(
            "/opt/flexedge", str(target)
        ).replace("/etc/systemd/system/flexedge-node.service", str(unit)))
        systemctl.write_text('''#!/bin/sh
echo "$1" >> "$TEST_ROOT/service-calls"
case "$1" in
    show) echo loaded ;;
    is-enabled) exit 1 ;;
    enable) exit 48 ;;
    stop)
        if [ "$TEST_FAILURE" = restore ] && [ -f "$TEST_ROOT/stopped" ]; then exit 49; fi
        touch "$TEST_ROOT/stopped"
        ;;
    *) exit 0 ;;
esac
''')
        for failure in ("enable", "restore"):
            (Path(root) / "stopped").unlink(missing_ok=True)
            calls.write_text("")
            env.update(TEST_FAILURE=failure, TEST_TARGET=str(target))
            result = run_installer(isolated, env)
            if failure == "enable":
                if result.returncode != 48 or "disable" not in calls.read_text().splitlines():
                    raise AssertionError("enable failure did not restore disabled state")
                if (target / "node").read_text() != "old binary" or unit.read_text() != "old unit":
                    raise AssertionError("enable failure did not restore old files")
                if list(Path(root).glob("staging.*")):
                    raise AssertionError("enable failure left staging files")
            else:
                backups = list(Path(root).glob("staging.*/backup/0"))
                if result.returncode != 1 or len(backups) != 1:
                    raise AssertionError("restore failure did not preserve backup")
                if backups[0].read_text() != "old binary":
                    raise AssertionError("restore failure backup is not original binary")
                if os.fsencode(backups[0].parent.parent) not in result.stderr:
                    raise AssertionError("restore failure did not report recovery path")
        print("enable failure rollback and recovery failure backup retention passed")
        successful = Path(root) / "successful"
        success_unit = Path(root) / "success-unit"
        isolated.write_text(Path(os.fsdecode(installer)).read_text().replace(
            "/opt/flexedge", str(successful)
        ).replace("/etc/systemd/system/flexedge-node.service", str(success_unit)))
        systemctl.write_text('''#!/bin/sh
echo "$1" >> "$TEST_ROOT/service-calls"
case "$1" in
    show) echo not-found ;;
    is-enabled) exit 1 ;;
    is-active) [ -f "$TEST_ROOT/started" ] ;;
    restart) touch "$TEST_ROOT/started" ;;
    *) exit 0 ;;
esac
''')
        calls.write_text("")
        env.update(TEST_FAILURE="none", TEST_TARGET=str(successful))
        retained = set(Path(root).glob("staging.*"))
        result = run_installer(isolated, env)
        if result.returncode != 0:
            raise AssertionError(f"successful installation: {result.returncode}, {result.stderr!r}")
        if (successful / "node").read_bytes() != b"test binary":
            raise AssertionError("successful installation has wrong binary")
        if (successful / "credentials").read_text() != "node_id=" + "a" * 32 + "\nsecret=" + "!" * 32 + "\n":
            raise AssertionError("successful installation has wrong credentials")
        for name, mode in (("node", 0o755), ("credentials", 0o600), ("state", 0o700)):
            if (successful / name).stat().st_mode & 0o777 != mode:
                raise AssertionError(f"successful installation has wrong {name} permissions")
        actions = calls.read_text().splitlines()
        if "stop" in actions or actions.index("restart") > actions.index("enable"):
            raise AssertionError("successful installation has wrong service order")
        if f"ExecStart={successful}/node wss://example.invalid" not in success_unit.read_text():
            raise AssertionError("successful installation has wrong service command")
        if set(Path(root).glob("staging.*")) != retained:
            raise AssertionError("successful installation left staging files")
        print("successful installation files, permissions and service order passed")
        failed_enable = Path(root) / "failed-enable"
        failed_unit = Path(root) / "failed-unit"
        isolated.write_text(Path(os.fsdecode(installer)).read_text().replace(
            "/opt/flexedge", str(failed_enable)
        ).replace("/etc/systemd/system/flexedge-node.service", str(failed_unit)))
        systemctl.write_text('''#!/bin/sh
case "$1" in
    show) echo not-found ;;
    is-enabled) exit 1 ;;
    is-active) [ -f "$TEST_UNIT" ] ;;
    enable) exit 48 ;;
    disable) [ -f "$TEST_UNIT" ] ;;
    *) exit 0 ;;
esac
''')
        env.update(TEST_TARGET=str(failed_enable), TEST_UNIT=str(failed_unit))
        result = run_installer(isolated, env)
        if result.returncode != 48:
            raise AssertionError(f"new install enable rollback: {result.returncode}")
        if failed_unit.exists() or (failed_enable / "node").exists():
            raise AssertionError("new install enable rollback left installed resources")
        if set(Path(root).glob("staging.*")) != retained:
            raise AssertionError("new install enable rollback left staging")


if __name__ == "__main__":
    main()
