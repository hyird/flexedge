"""Exercise the real CLI with isolated synthetic IPv4/IPv6 XDB fixtures."""
import ipaddress
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


def fixture(version):
    start = ipaddress.ip_address("1.2.3.0" if version == 4 else "2001:db8::")
    end = ipaddress.ip_address("1.2.3.255" if version == 4 else "2001:db8::ffff")
    size = 14 if version == 4 else 38
    offset = 256 + 256 * 256 * 8
    region = b"Country|Province|City|123|Network"
    data = bytearray(offset)
    struct.pack_into("<IIHH", data, 8, offset, offset, version, 4)
    bucket = (start.packed[0] * 256 + start.packed[1]) * 8 + 256
    struct.pack_into("<II", data, bucket, offset, offset)
    addresses = start.packed[::-1] + end.packed[::-1] if version == 4 else start.packed + end.packed
    data += addresses + struct.pack("<HI", len(region), offset + size) + region
    return data, bucket, offset


def main():
    with tempfile.TemporaryDirectory(prefix="flexedge-xdb-") as directory:
        for version, inside, outside in ((4, "1.2.3.255", "1.2.4.0"),
                                         (6, "2001:db8::ffff", "2001:db8::1:0")):
            data, bucket, offset = fixture(version)
            path = Path(directory) / f"v{version}.xdb"
            def check(ip, expected, text=""):
                result = subprocess.run([sys.argv[1], f"--v{version}", str(path), "--ip", ip],
                                        capture_output=True, text=True, timeout=5)
                if result.returncode != expected or (text and text not in result.stdout):
                    raise AssertionError(f"v{version} {ip}: exit={result.returncode}")
            path.write_bytes(data)
            check(inside, 0, "Country")
            check(outside, 2)
            check("invalid-ip", 2)
            struct.pack_into("<II", data, bucket, offset + 1, offset + 1)
            path.write_bytes(data)
            check(inside, 2)
            data, bucket, offset = fixture(version)
            size = 14 if version == 4 else 38
            forged = len(data)
            data += data[offset:offset + size]
            struct.pack_into("<II", data, bucket, forged, forged)
            path.write_bytes(data)
            check(inside, 2)
    print("Synthetic XDB IPv4/IPv6 lookup, range and invalid-input checks passed")


if __name__ == "__main__":
    main()
