export type StatusTone = 'success' | 'warning' | 'danger' | 'accent' | 'default'

/** Chinese label and tone of one wire status value. Tones carry meaning only
 * together with the label text; never rely on color alone. */
export type StatusDescriptor = { label: string; tone: StatusTone }

/** A domain's wire status → descriptor dictionary (`*-display.ts`); list
 * filters and status chips read the same dictionary. */
export type StatusDictionary<K extends string = string> = Readonly<
  Record<K, StatusDescriptor>
>

/** Descriptor of `value` in a domain dictionary; a value from a newer backend
 * keeps its raw text in a neutral chip. */
export function describeStatus(
  dictionary: StatusDictionary,
  value: string | null | undefined
): StatusDescriptor {
  if (value && Object.hasOwn(dictionary, value)) return dictionary[value]
  return { label: value || '未知', tone: 'default' }
}

/** Every label of the given dictionaries, for `StatusChip`'s `reserve`. */
export function statusReserve(...dictionaries: StatusDictionary[]) {
  return [
    ...new Set(
      dictionaries.flatMap((dictionary) =>
        Object.values(dictionary).map((status) => status.label)
      )
    ),
  ]
}

/** Wire values of a dictionary in declaration order, e.g. for `z.enum`. */
export function statusIds<K extends string>(dictionary: StatusDictionary<K>) {
  return Object.keys(dictionary) as [K, ...K[]]
}

/** List filter options (`{ id, label }`) of a dictionary. */
export function statusOptions<K extends string>(
  dictionary: StatusDictionary<K>
) {
  return statusIds(dictionary).map((id) => ({
    id,
    label: dictionary[id].label,
  }))
}

/** The enabled/disabled switch every configurable resource shares. */
export const enablementStatuses = {
  enabled: { label: '已启用', tone: 'success' },
  disabled: { label: '已停用', tone: 'default' },
} as const satisfies StatusDictionary<'enabled' | 'disabled'>
