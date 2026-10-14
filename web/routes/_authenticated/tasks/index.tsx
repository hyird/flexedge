import { createFileRoute } from '@tanstack/react-router'
import { tasksSearchSchema } from '@/features/tasks/search'
import { TasksPage } from '@/features/tasks/tasks-page'

export const Route = createFileRoute('/_authenticated/tasks/')({
  validateSearch: tasksSearchSchema,
  staticData: { breadcrumb: '任务中心' },
  component: TasksPage,
})
