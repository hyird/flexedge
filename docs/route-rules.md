# Website route rules

The v0.3.35 release adds the first group of route controls. These are persisted in
the existing website configuration JSON; no database table migration is required.

## Match and priority

- `name` (up to 100 UTF-8 bytes) and `description` (1000 bytes) are management metadata.
- `hostnames` is an optional list of exact DNS names. Empty means all domains of
  this website. Incoming Host / HTTP/2 authority is compared without port or final
  dot, case-insensitively. This does not bind a domain or bypass website selection.
- Status, hostname and HTTP method filters are applied before path matching.
- Existing precedence remains: exact match, longest path of the same match type,
  then first rule on ties. A hostname filter does not confer additional priority.
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
without decoding `%2F`. No regular expressions or variables are interpreted.

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
preservation. New cluster manifests use schema v3. Older nodes reject v3 instead
of silently ignoring new semantics. New nodes can load existing v2 manifests.

Deploy upgraded node binaries before exposing the new editor; verify every node
has converged after the server upgrade. Keep binary and configuration backups.
Do not roll the server back after saving new rule options without also restoring
the corresponding pre-upgrade website configuration, since old software does not
understand the added fields. Preview is local form simulation, not a node health
or release acknowledgement check.
