# Website route rules

Version 0.3.38 uses Cloudflare-style phases: **Single Redirects -> URL Rewrite -> Origin Rules**.
The configuration remains in `route_rules`; `action` is `redirect`, `rewrite`, or `proxy`.
Rules retain their list order within each phase, regardless of interleaving between actions.

## Match and execution order

- Redirects match the incoming URL. The first matching redirect ends the request.
- Rewrites all match the URL and headers at phase entry. They do not match previous
  rewrites. Path and query changes accumulate independently; the last matching change
  to each field wins. `none` and `preserve` leave that field unchanged, including changes
  selected by earlier rules. No rewrite loop or redirect re-evaluation occurs.
- Origin (`proxy`) rules match the final rewritten URL, including its query, and the
  original headers. All matching rules apply: the last source-group selection wins;
  request and response headers merge case-insensitively by name, with the last value
  winning. Unrelated headers from earlier rules remain. No match uses the default group.
- Existing proxy rules with rewrite fields contribute to both phases independently:
  their rewrite matches the incoming URL, while their origin settings match the final
  URL. To route `/old` rewritten to `/api`, use a rewrite on `/old` and an origin rule
  on `/api`. A combined rule on `/old` will not select its source if the final URL no
  longer matches `/old`.
- Place general origin defaults before specific overrides. Redirect fallback rules
  belong last. Match type and path length do not grant extra priority.
- Enabled status, exact hostname, HTTP method and all request conditions must match.
  Hostnames ignore case, port and a trailing dot; they do not bind domains or change
  website selection. Prefixes respect path boundaries (`/api` does not match `/apix`).
- Website-level force HTTPS still runs before this pipeline for plain HTTP requests.
  This implements the three named phases, not Cloudflare's full security/cache pipeline
  or its separate Bulk Redirects feature.

Reference: https://developers.cloudflare.com/rules/origin-rules/#execution-order
and https://developers.cloudflare.com/rules/transform/url-rewrite/ .

## Path processing (rewrite action and existing proxy rewrite fields)

| rewrite_mode | /api/users with matched path /api | rewrite_path |
| --- | --- | --- |
| none | /api/users | empty |
| replace_path | /internal | /internal |
| strip_prefix | /users | empty |
| replace_prefix | /v2/users | /v2/ |

Prefix modes require prefix matching. Boundary slashes are joined once; removing
the complete path yields `/`. Matching and rewriting operate on the encoded path,
without decoding `%2F`. Regex capture templates are described below.
Replacement paths cannot contain whitespace, `?` or `#`; configure query changes
through the independent query policy instead of embedding them in the path.

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

Cluster manifests use schema v6. Older nodes reject v6 instead of silently applying
v5 first-match behavior. Upgraded nodes can read v2-v5 snapshots but immediately use
phase semantics for their stored order. This is a behavior change even before a new
configuration is saved; review overlapping rules and combined proxy/rewrite rules first.

No database migration or automatic rule reordering is performed. The editor supports
standalone URL rewrites without a source group. Saving new actions requires the updated
server and nodes. Back up website configurations and binaries, upgrade nodes before
publishing the updated server/editor, and verify every node has converged. Rollback
requires the previous server/node binaries and the corresponding configuration backup.
Do not deploy or roll back only the editor to change execution semantics.

Local preview reports the matching rewrite and origin rule numbers. It is a form
simulation, not a live origin request or confirmation that nodes applied the configuration.
