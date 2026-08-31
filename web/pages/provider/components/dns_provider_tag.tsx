import { Badge } from '@/components/ui/badge';
import { DNS_PROVIDER_REGISTRY, type DnsProviderCode } from '@/config/providers';

interface DnsProviderTagProps {
    provider: DnsProviderCode;
}

export default function DnsProviderTag({ provider }: DnsProviderTagProps) {
    return (
        <Badge variant={provider === 'cloudflare' ? 'warning' : 'info'}>
            {DNS_PROVIDER_REGISTRY[provider].label}
        </Badge>
    );
}
