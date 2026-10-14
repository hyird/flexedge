/** Validation issues (e.g. zod `error.issues`) as one message per top-level
 * field, keyed by `String(path[0])`. The first issue wins, so a field shows its
 * most basic failure (`至少 8 个字符`) before later refinements. Issues without a
 * path are ignored; show those as the form-level error. */
export function fieldErrors(
  issues: readonly { path: readonly PropertyKey[]; message: string }[]
): Record<string, string> {
  const errors: Record<string, string> = {}
  for (const issue of issues) {
    if (issue.path.length === 0) continue
    const field = String(issue.path[0])
    errors[field] ??= issue.message
  }
  return errors
}
