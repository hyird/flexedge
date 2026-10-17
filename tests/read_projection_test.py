"""Run real PostgreSQL projections with missing and synthetic GeoIP databases."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True

from support.xdb_fixture import fixture


IPV4_RANGES = (
    ("1.2.3.0", "1.2.3.255", "中国|广东省|广州市|AS4134|Chinanet"),
    ("1.2.4.0", "1.2.4.255", "中国|广东省|广州市||Organization without ASN"),
    ("1.2.7.0", "1.2.7.255", "中国|浙江省|杭州市|AS4134|Z Legacy Chinanet Name"),
    ("1.2.8.0", "1.2.8.255", "中国|陕西省|西安市|AS4134|"),
) + tuple(
    (f"1.2.{index}.0", f"1.2.{index}.255",
     f"中国|||AS650{index}|" + (
         "A Complete Autonomous System Organization Name" if index == 21 else f"Network {index}"))
    for index in range(10, 22)
)
IPV6_RANGES = (("2001:db8::", "2001:db8::ffff", "美国|||AS4134|Chinanet"),)


def main():
    with tempfile.TemporaryDirectory(prefix="flexedge-read-projection-xdb-") as directory:
        ipv4_path = Path(directory) / "ip2region_v4.xdb"
        ipv6_path = Path(directory) / "ip2region_v6.xdb"
        ipv4_path.write_bytes(fixture(4, IPV4_RANGES))
        ipv6_path.write_bytes(fixture(6, IPV6_RANGES))
        for mode in ("missing-xdb", "with-xdb"):
            environment = os.environ.copy()
            environment["FLEXEDGE_XDB_V4_PATH"] = str(ipv4_path)
            environment["FLEXEDGE_XDB_V6_PATH"] = str(ipv6_path)
            result = subprocess.run([sys.argv[1], mode], env=environment, capture_output=True,
                                    text=True, timeout=25)
            if result.returncode == 77:
                return 77
            if result.returncode != 0:
                raise AssertionError(f"{mode}: exit={result.returncode}\n"
                                     f"{result.stdout}{result.stderr}")
            print(f"{mode}: {result.stdout.strip()}")
    print("Dashboard ASN aggregation and PostgreSQL projection checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
