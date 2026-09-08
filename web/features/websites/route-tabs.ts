export const routeTabs = {
  redirect: {
    title: 'URL 跳转',
    description: '匹配原始 URL，按列表顺序执行，首条命中即返回跳转。',
    empty: '暂无 URL 跳转规则。',
  },
  rewrite: {
    title: 'URL 重写',
    description:
      '匹配原始 URL，按列表顺序修改路径和查询参数，同项设置以后面的规则为准。',
    empty: '暂无 URL 重写规则。',
  },
  proxy: {
    title: '路由规则',
    description:
      '匹配重写后的 URL，按列表顺序选择源站并设置请求头，同项设置以后面的规则为准。',
    empty: '暂无路由规则，将使用默认源站组。',
  },
} as const
