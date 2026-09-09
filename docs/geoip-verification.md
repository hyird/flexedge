# GeoIP reader verification

`XdbDatabase` receives IPv4 and IPv6 paths explicitly. The runtime adapter owns
environment lookup; `xdb_lookup` passes CLI paths directly without changing the
process environment. Vector pointers must remain inside the declared record index
and be aligned to record boundaries.

The packaged systemd unit injects `FLEXEDGE_XDB_V4_PATH` and
`FLEXEDGE_XDB_V6_PATH` as process environment variables, matching the CMake install
locations `/opt/flexedge/geo/ip2region_v4.xdb` and
`/opt/flexedge/geo/ip2region_v6.xdb`. These values are not read from the application's
dotenv object. When launching the server directly, explicitly provide the process
environment paths. The runtime database is initialized on first access and retains
its readers for the process lifetime; replace data files and restart the server to
load a new dataset. Missing or unreadable files disable that address family's
lookup without preventing startup.

`tests/xdb_lookup_test.py` runs synthetic IPv4/IPv6 fixtures through the actual
CLI, covering inclusive range ends, missing ranges, invalid addresses, misaligned
pointers and pointers to forged records outside the declared index. The suite
passed on Linux and Windows and is registered in Linux CTest.

The Windows CLI also queried the retained deployment files under
`build/deploy/xdb-20260906T092300/`:

| File | Bytes | SHA-256 |
| --- | ---: | --- |
| ip2region_v4.xdb | 71424108 | 9fd2b69c6cc6e5ed88216e82c372d386b60fb77e7177e066cdebb96c64976226 |
| ip2region_v6.xdb | 57369773 | a53d1db709064e927fb3503398ce5a50dbcc97a2cf360af6123f8c39c5b42350 |

Queries returned `中国 · 浙江省 · 杭州市` for `223.5.5.5`,
`美国 · 谷歌云 · AS15169` for `8.8.8.8`, and `中国 · 浙江 · 杭州` for
`2400:3200::1`. `1.1.1.1` returned no location; independent inspection of its
raw record found `|||||Cloudflare|AS13335`. The location parser requires a known
country, so this result follows the stored data and current display policy.

These are retained deployment files, not a verification of the latest upstream
release. The checks establish sample lookup behavior, not full-range accuracy or
geographic correctness of the source data.
