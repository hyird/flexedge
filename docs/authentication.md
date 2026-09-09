# Authentication boundaries and verification

Authentication uses the `flexedge_session` cookie and PostgreSQL session records.
The cookie carries a session identifier and secret; the database stores the secret
hash. Refresh rotates the credential within its session family. Reusing a revoked
credential with the matching secret revokes the family.

## Responsibilities

| Layer | Source | Responsibility |
| --- | --- | --- |
| HTTP routes | `service/domains/auth/auth.controller.h` | Route validation, response envelopes and endpoint assembly |
| HTTP application adapters | `auth.service.h`, `auth_session.service.h` | Map authentication outcomes to HTTP errors, DTOs and cookies |
| Authentication decision | `authenticate.service.h` | Password verification, login throttling and disabled-user rejection; returns a user or typed failure without HTTP Context |
| User storage | `auth_user.store.h` | User SQL and database row conversion |
| Throttle storage | `login_throttle.store.h` | Failure counters, lock expiry and per-user reset |
| Session storage | `auth_session.store.h` | Session creation, transactional rotation and family revocation |
| Request authentication | `auth_session_read.service.h`, `service/middleware/auth.h` | Resolve a principal from storage, then bind it to the request |
| Models | `auth_user.h`, `session_credential.h` | User data and sensitive session credentials without database dependencies |
| Configuration | `service/config/auth.h` | Parse explicit environment input into session lifetime and cookie security settings |
| Cookie adapter | `service/domains/auth/session_cookie.h` | Cookie parsing, encoding, attributes and deletion |

Paths without a prefix above are relative to `service/domains/auth/`.
Storage accepts a database handle rather than HTTP Context. Authentication
decisions do not construct response DTOs or mutate cookies. Database and hashing
failures remain exceptions; expected authentication rejection uses typed results.

## Verified behavior

- Missing users still incur a password hash comparison before rejection.
- The fifth failed login in the active window locks the identity for 15 minutes.
  During the lock even a correct password is rejected. An expired window starts a
  new failure count; a successful login clears that identity's failures.
- Deleted users and tenants, and disabled tenants, do not satisfy user queries.
  Disabled user status reaches the authentication decision for explicit rejection.
- Refresh commits revocation of the prior credential and insertion of its
  replacement together. Cookie replacement follows the commit.
- Logout only revokes a family when the presented identifier and secret hash
  match. It also clears the browser cookie.
- Configuration defaults to seven days and secure cookies. Cookie security accepts
  only `true` or `false`; session duration must be positive.

## Reproducing verification

`flexedge_auth_configuration` exercises defaults, overrides and invalid values
through the real dotenv loader. `flexedge_architecture` checks dependency
boundaries. Both are registered with CTest.

`tests/auth_session_sql_test.py` extracts current SQL from the authentication
stores and table definitions from the schema. Run it against an empty isolated
PostgreSQL database with loopback `PGHOST` and a database name ending in `_auth_qa`.
Set `PGPORT`, `PGUSER` and, when needed, `PGPASSWORD` and `PSQL`. All table and fixture
writes are inside a transaction that rolls back. This validates SQL behavior,
not C++ coroutine execution or HTTP responses.

`tests/auth_http_test.py` exercises the actual server:

```text
python tests/auth_http_test.py http://127.0.0.1:51102
```

Set `AUTH_QA_PASSWORD` to the isolated server's admin fixture password. Use a
separate runtime directory and database, a dedicated Redis instance and explicit
loopback ports. Set `AUTH_COOKIE_SECURE=false` only in this local HTTP fixture.
Full server migrations require TimescaleDB to be installed and preloaded in the
test PostgreSQL instance. Do not reuse production credentials or runtime files.

The HTTP test covers login, cookie scope/HttpOnly, current-user access, refresh,
old-credential replay, simultaneous refresh of one credential, family revocation,
logout and lockout. The concurrent scenario requires one success and one rejection,
then verifies that the winning response's new credential is also revoked by the
replay policy. Its final scenario
locks the admin fixture: reset that fixture's throttle record in the isolated
database or recreate the database before another run. Shut down the owned test
processes after verification.

The current Windows server passed this HTTP suite with real PostgreSQL,
TimescaleDB and Redis. This is backend integration evidence; browser UI,
load testing and production deployment are separate checks.
