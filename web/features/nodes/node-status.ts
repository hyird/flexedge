import { statusLabels } from '@/lib/status'

// Every label each node status slot can show (`StatusChip` `reserve`), so a
// state change never moves neighbouring content. Connection follows
// service/domains/node/node_read_service.h (unregistered / online / offline).
export const connectionLabels = statusLabels([
  'online',
  'offline',
  'unregistered',
])
export const nodeStatusLabels = statusLabels(['enabled', 'disabled'])
export const registrationLabels = ['等待接入', ...statusLabels(['registered'])]
