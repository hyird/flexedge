import type { LucideIcon } from 'lucide-react'
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card'

type Props = {
  label: string
  value: string | number
  hint: string
  icon: LucideIcon
}

export function MetricCard({ label, value, hint, icon: Icon }: Props) {
  return (
    <Card>
      <CardHeader className='flex flex-row items-center justify-between space-y-0 pb-2'>
        <CardTitle className='text-sm font-medium'>{label}</CardTitle>
        <Icon className='size-4 text-muted-foreground' />
      </CardHeader>
      <CardContent>
        <div className='text-2xl font-bold'>{value}</div>
        <p className='text-xs text-muted-foreground'>{hint}</p>
      </CardContent>
    </Card>
  )
}
