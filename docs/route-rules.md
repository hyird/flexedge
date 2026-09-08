# Website route rules

The v0.3.36 release includes both groups of route controls. These are persisted in
the existing website configuration JSON; no database table migration is required.

## Match and priority

- `name` (up to 100 UTF-8 bytes) and `description` (1000 bytes) are management metadata.
- `hostnames` is an optional list of exact DNS names. Empty means all domains of
  this website. Incoming Host / HTTP/2 authority is compared without port or final
  dot, case-insensitively. This does not bind a domain or bypass website selection.
- Status, hostname and HTTP method filters are applied before path matching.
- Precedence is exact, prefix, suffix, then regex. Within exact/prefix/suffix,
  the longest path wins, followed by the first rule on ties. Regex rules use
  the first matching rule regardless of pattern length. Hostname and request
  conditions do not confer additional priority. An existing catch-all prefix
  `/` therefore outranks regex rules; use the default origin group for fallback
  if regex rules must remain reachable.
- Prefix matches respect path-segment boundaries, so `/api` does not match `/apix`.

## Path processing (proxy action only)

| rewrite_mode | /api/users with matched path /api | rewrite_path |
| --- | --- | --- |
| none | /api/users | empty |
| replace_path | /internal | /internal |
| strip_prefix | /users | empty |
| replace_prefix | /v2/users | /v2/ |

Prefix modes require prefix matching. Boundary slashes are joined once; removing
the complete path yields `/`. Matching and rewriting operate on the encoded path,
without decoding `%2F`. Regex capture templates are described below.

## Request conditions

`conditions` is an optional array, with at most 20 entries. Each entry has
`source` (`header` or `query`), `name` (1–256 UTF-8 bytes), `op`, and `value`
(0–2048 bytes). All entries must match (AND); no entries means no extra filter.
Supported operators are `equals`, `not_equals`, `exists`, and `absent`. The latter
two require an empty value. Empty values are meaningful for equality.

Header names compare case-insensitively; values compare case-sensitively after
trimming HTTP optional whitespace. Matching uses the original inbound request,
before request-header mutations. HTTP/2 `:authority` is exposed as `Host`.
Repeated header lines are evaluated separately, not comma-split.

Query names and values compare case-sensitively after percent/UTF-8 decoding;
`+` is a space, `%2B` is a plus, and `?flag` has an empty value. Repeated fields
match equality when any value equals; inequality requires presence and every
value to differ. Malformed percent encoding or invalid UTF-8 makes all query
conditions fail, including `absent`; header-only conditions remain usable.
Matching does not change the query sent to the origin; the query policy does.

## Suffix, regex and captures

- `suffix` compares the encoded path ending (for example `.jpg`), not the query.
- `regex` uses RE2, compiled when a configuration is loaded. Patterns are limited
  to 512 UTF-8 bytes, 9 capturing groups and a 1 MiB engine memory budget per
  expression. Lookaround, backreferences and `\C` are not supported. A match may
  cover part of the path unless anchored with `^` and `$`.
- For regex rules, whole-path replacement (`replace_path`) and redirect URLs
  support `${0}` (the matched text) and `${1}` through `${9}` (existing groups).
  Optional unmatched groups expand to empty; `$$` is a literal dollar sign.
  Nonexistent/malformed group references are rejected before configuration save.
  The template replaces the whole destination, not just the matching substring.
- Prefix stripping/replacement still requires `prefix`, not `suffix` or `regex`.
  Query replacements remain literal strings and do not interpolate captures.
- Expansion is limited to 16 KiB and cannot inject control characters or spaces.
  Invalid/oversized expansion returns an error response rather than forwarding
  an unsafe request or Location header.
- Browser previews use the RE2JS port, not native JavaScript backtracking regex.
  The server remains authoritative for validation, including the native engine
  memory budget. Preview is not an authenticated origin request.

Example: pattern `^/old/([^/]+)/(.*)$`, destination `/new/${2}/${1}` transforms
`/old/books/a%2Fb` into `/new/a%2Fb/books` without decoding the encoded slash.

## Redirect status

301, 302, 307 and 308 are supported. 307 (temporary) and 308 (permanent) instruct
the client to retain the request method and body when following Location. The
edge returns the redirect; it does not replay the request to the destination.

## Query processing (proxy and redirect actions)

- `preserve`: keep a destination's explicit query, otherwise append the incoming
  query. This retains existing redirect behavior when the destination has parameters.
- `drop`: remove destination and incoming query strings.
- `replace`: replace all query parameters with the literal `query_string` (empty
  means clear). Do not include a leading `?`; encode special characters.
- Redirect fragments remain at the end, after query parameters.

For example `/api/users?a=1` with `replace_prefix=/v2` and `replace query=b=2`
becomes `/v2/users?b=2`.

## Upgrade and rollback

Stored rules without the added fields normalize to empty metadata and hostname
filters, the original whole-path rewrite when a rewrite path exists, and query
preservation and no conditions. New cluster manifests use schema v4. Older nodes
reject v4 instead of silently ignoring new semantics. New nodes can load existing
v2 and v3 manifests.

Deploy upgraded node binaries before exposing the new editor; verify every node
has converged after the server upgrade. Keep binary and configuration backups.
Do not roll the server back after saving new rule options without also restoring
the corresponding pre-upgrade website configuration, since old software does not
understand the added fields. Preview is local form simulation, not a node health
or release acknowledgement check.
