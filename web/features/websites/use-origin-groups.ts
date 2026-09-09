import { useMemo } from 'react'
import { useFieldArray, useWatch, type UseFormReturn } from 'react-hook-form'
import { toast } from 'sonner'
import {
  collectOriginGroups,
  nextOriginGroupName,
  originRemovalError,
  renameOriginGroup,
} from './origin-groups'
import type { WebsiteFormValues } from './website-form'

// This adapter applies domain operations without replacing RHF field-array identities.
export function useOriginGroups(form: UseFormReturn<WebsiteFormValues>) {
  const origins = useFieldArray({
    control: form.control,
    name: 'origins',
    keyName: 'formKey',
  })
  const watchedOrigins = useWatch({ control: form.control, name: 'origins' })
  const originGroups = useMemo(
    () => collectOriginGroups(watchedOrigins),
    [watchedOrigins]
  )
  const selectableOriginGroups = useMemo(
    () => originGroups.filter((group) => group.enabledCount > 0),
    [originGroups]
  )
  const appendOrigin = (
    group: string,
    role: WebsiteFormValues['origins'][number]['role']
  ) => {
    origins.append({
      id: crypto.randomUUID(),
      group,
      protocol: 'http',
      host: '',
      port: 80,
      role,
      weight: 100,
      status: 'enabled',
    })
  }
  const rename = (currentName: string, draftName: string) => {
    const current = form.getValues()
    const result = renameOriginGroup(current, currentName, draftName)
    if (!result.ok) {
      toast.error(result.message)
      return false
    }
    const next = result.value
    const options = { shouldDirty: true, shouldValidate: true }
    next.origins.forEach((origin, index) => {
      if (origin.group !== current.origins[index].group)
        form.setValue(`origins.${index}.group`, origin.group, options)
    })
    if (next.default_origin_group !== current.default_origin_group)
      form.setValue('default_origin_group', next.default_origin_group, options)
    if (
      next.route_rules.some(
        (rule, index) => rule !== current.route_rules[index]
      )
    )
      form.setValue('route_rules', next.route_rules, options)
    return true
  }
  const removeOrigin = (index: number) => {
    const message = originRemovalError(form.getValues(), index)
    if (message) {
      toast.error(message)
      return
    }
    origins.remove(index)
  }
  return {
    origins,
    originGroups,
    selectableOriginGroups,
    appendOrigin,
    createOriginGroup: () =>
      appendOrigin(nextOriginGroupName(originGroups), 'primary'),
    renameOriginGroup: rename,
    removeOrigin,
  }
}
