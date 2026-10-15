"""Run the access-log mapper against synthetic five-field XDB files and without XDB files."""
import ipaddress
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

IPV4_RANGES = (
    ("1.2.3.0", "1.2.3.255", "中国|广东省|广州市|AS4134|Chinanet"),
    ("1.2.4.0", "1.2.4.255", "中国|广东省|广州市||"),
    ("1.2.5.0", "1.2.5.255", "中国|广东省|广州市|中国移动"),
    ("1.2.6.0", "1.2.6.255", "|||AS4294967295|" + "Very Long Autonomous System Name " * 8),
)
IPV6_RANGES = (("2001:db8::", "2001:db8::ffff", "美国|||AS15169|Google LLC"),)


def fixture(version, ranges):
    """Build an XDB whose ranges all share one vector-index bucket."""
    address_length = 4 if version == 4 else 16
    record_length = address_length * 2 + 6
    first_record = 256 + 256 * 256 * 8
    regions_offset = first_record + record_length * len(ranges)
    data = bytearray(first_record)
    last_record = first_record + record_length * (len(ranges) - 1)
    struct.pack_into("<IIHH", data, 8, first_record, last_record, version, 4)
    first = ipaddress.ip_address(ranges[0][0]).packed
    struct.pack_into("<II", data, (first[0] * 256 + first[1]) * 8 + 256, first_record, last_record)
    regions = bytearray()
    for start, end, region in ranges:
        start_bytes = ipaddress.ip_address(start).packed
        end_bytes = ipaddress.ip_address(end).packed
        if version == 4:
            start_bytes, end_bytes = start_bytes[::-1], end_bytes[::-1]
        encoded = region.encode("utf-8")
        data += start_bytes + end_bytes + struct.pack("<HI", len(encoded), regions_offset + len(regions))
        regions += encoded
    return bytes(data + regions)


def run(binary, mode, ipv4_path, ipv6_path):
    environment = os.environ.copy()
    environment["FLEXEDGE_XDB_V4_PATH"] = str(ipv4_path)
    environment["FLEXEDGE_XDB_V6_PATH"] = str(ipv6_path)
    result = subprocess.run([binary, mode], env=environment, capture_output=True, text=True,
                            timeout=10)
    if result.returncode != 0:
        raise AssertionError(f"{mode}: exit={result.returncode}\n{result.stdout}{result.stderr}")


def main():
    with tempfile.TemporaryDirectory(prefix="flexedge-access-log-xdb-") as directory:
        ipv4_path = Path(directory) / "ip2region_v4.xdb"
        ipv6_path = Path(directory) / "ip2region_v6.xdb"
        ipv4_path.write_bytes(fixture(4, IPV4_RANGES))
        ipv6_path.write_bytes(fixture(6, IPV6_RANGES))
        run(sys.argv[1], "with-xdb", ipv4_path, ipv6_path)
        run(sys.argv[1], "missing-xdb", Path(directory) / "absent_v4.xdb",
            Path(directory) / "absent_v6.xdb")
    print("Access-log ASN mapping checks passed")


if __name__ == "__main__":
    main()
