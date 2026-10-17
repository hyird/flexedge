"""Synthetic XDB data for database projection integration tests."""
import ipaddress
import struct


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
