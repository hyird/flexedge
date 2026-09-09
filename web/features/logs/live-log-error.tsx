import { AlertCircle } from 'lucide-react'
import { Alert, AlertDescription } from '@/components/ui/alert'

export function LiveLogError({ error }: { error: string | null }) {
  if (!error) return null
  return (
    <Alert variant='destructive' className='mx-4 w-auto shrink-0'>
      <AlertCircle />
      <AlertDescription>{error}</AlertDescription>
    </Alert>
  )
}
