const dateTimeFormatter = new Intl.DateTimeFormat('zh-CN', {
    year: 'numeric',
    month: '2-digit',
    day: '2-digit',
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit',
    hour12: false,
    hourCycle: 'h23',
});

export function formatDateTime(value?: string | null, fallback = '—') {
    if (!value) return fallback;
    const normalized = value.replace(/(\.\d{3})\d+/, '$1').replace(/([+-]\d{2})$/, '$1:00');
    const date = new Date(normalized);
    if (Number.isNaN(date.getTime())) return value;
    return dateTimeFormatter.format(date).replaceAll('/', '-');
}
