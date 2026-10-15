import { NumberValue } from '@heroui-pro/react'
import { KPI } from '@heroui-pro/react/kpi'
import { scaleBitsPerSecond, scaleBytes } from '@/lib/format'

/** `KPI.Value` for a byte size or a `*_bps` rate: the amount scaled to its
 * display unit, followed by the unit as a NumberValue suffix (`KPI.Value` only
 * renders a function child). A missing value shows `—` in the same slot. */
export function KpiQuantity({
  value,
  unit,
}: {
  value?: number
  unit: 'bytes' | 'bps'
}) {
  if (value === undefined || !Number.isFinite(value))
    return <KPI.Value value={0}>{() => '—'}</KPI.Value>
  const scaled =
    unit === 'bytes' ? scaleBytes(value) : scaleBitsPerSecond(value)
  return (
    <KPI.Value
      value={scaled.amount}
      minimumFractionDigits={scaled.fractionDigits}
      maximumFractionDigits={scaled.fractionDigits}
      className='whitespace-nowrap'
    >
      {(formatted) => (
        <>
          {formatted}
          <NumberValue.Suffix className='ms-1 text-base font-medium text-muted'>
            {scaled.unit}
          </NumberValue.Suffix>
        </>
      )}
    </KPI.Value>
  )
}
