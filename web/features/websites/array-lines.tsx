import { useState } from 'react'
import { TextAreaField } from '@/components/forms'
import { linesToValues } from './website-form'

export function ArrayLines({
  label,
  value,
  onChange,
  hint,
  error,
}: {
  label: string
  value: string[]
  onChange: (value: string[]) => void
  hint?: string
  error?: string
}) {
  const [text, setText] = useState(value.join('\n'))
  return (
    <TextAreaField
      label={label}
      value={text}
      onChange={(next) => {
        setText(next)
        onChange(linesToValues(next))
      }}
      hint={hint}
      error={error}
      rows={4}
    />
  )
}
