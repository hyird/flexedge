import { ArrowDown, ArrowUp, Check, LoaderCircle, Plus, Search, X } from 'lucide-react';
import { Segmented } from 'antd';
import { type ReactNode, useId, useState } from 'react';
import { Badge } from '@/components/ui/badge';
import { Button } from '@/components/ui/button';
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '@/components/ui/card';
import { Checkbox } from '@/components/ui/checkbox';
import { Input } from '@/components/ui/input';
import { Label } from '@/components/ui/label';
import { Switch } from '@/components/ui/switch';
import { cn } from '@/lib/utils';

interface SectionCardProps {
    title: ReactNode;
    description?: ReactNode;
    action?: ReactNode;
    children: ReactNode;
    className?: string;
}

export function SectionCard({ title, description, action, children, className }: SectionCardProps) {
    return (
        <Card className={cn('gap-4', className)}>
            <CardHeader className="grid grid-cols-[minmax(0,1fr)_auto] gap-x-4">
                <CardTitle>{title}</CardTitle>
                {description && <CardDescription>{description}</CardDescription>}
                {action && <div className="row-span-2 row-start-1 self-start">{action}</div>}
            </CardHeader>
            <CardContent className="space-y-4">{children}</CardContent>
        </Card>
    );
}

interface SwitchRowProps {
    id: string;
    label: ReactNode;
    description?: ReactNode;
    checked: boolean;
    onCheckedChange: (checked: boolean) => void;
    disabled?: boolean;
}

export function SwitchRow({
    id,
    label,
    description,
    checked,
    onCheckedChange,
    disabled,
}: SwitchRowProps) {
    return (
        <div className="flex min-h-12 items-center justify-between gap-4 rounded-lg border bg-background px-3 py-2.5">
            <div className="min-w-0">
                <Label htmlFor={id} className="cursor-pointer">
                    {label}
                </Label>
                {description && (
                    <p className="mt-1 text-xs leading-relaxed text-muted-foreground">
                        {description}
                    </p>
                )}
            </div>
            <Switch
                id={id}
                checked={checked}
                onCheckedChange={onCheckedChange}
                disabled={disabled}
            />
        </div>
    );
}

interface ChoiceOption<T extends string> {
    value: T;
    label: ReactNode;
}

interface ChoiceGroupProps<T extends string> {
    value: T;
    options: Array<ChoiceOption<T>>;
    onValueChange: (value: T) => void;
    disabled?: boolean;
    className?: string;
    id?: string;
    'aria-label'?: string;
    'aria-labelledby'?: string;
    'aria-describedby'?: string;
}

export function ChoiceGroup<T extends string>({
    value,
    options,
    onValueChange,
    disabled,
    className,
    id,
    'aria-label': ariaLabel,
    'aria-labelledby': ariaLabelledBy,
    'aria-describedby': ariaDescribedBy,
}: ChoiceGroupProps<T>) {
    return (
        <Segmented<T>
            id={id}
            aria-label={ariaLabel}
            aria-labelledby={ariaLabelledBy}
            aria-describedby={ariaDescribedBy}
            block
            value={value}
            options={options}
            disabled={disabled}
            className={className}
            onChange={onValueChange}
        />
    );
}

interface TagEditorProps {
    value: string[];
    onChange: (value: string[]) => void;
    placeholder?: string;
    suggestions?: string[];
    disabled?: boolean;
    invalid?: boolean;
}

export function TagEditor({
    value,
    onChange,
    placeholder = '输入后按 Enter 添加',
    suggestions = [],
    disabled,
    invalid,
}: TagEditorProps) {
    const [draft, setDraft] = useState('');
    const suggestionListId = useId();

    const addDraft = () => {
        const additions = draft
            .split(/[,\s]+/)
            .map((item) => item.trim())
            .filter(Boolean);
        if (additions.length === 0) return;
        onChange([...value, ...additions.filter((item) => !value.includes(item))]);
        setDraft('');
    };

    return (
        <div
            className={cn(
                'rounded-md border border-input bg-background p-2 shadow-xs',
                invalid && 'border-destructive ring-3 ring-destructive/20',
                disabled && 'opacity-50'
            )}
        >
            <div className="mb-2 flex min-h-6 flex-wrap gap-1.5">
                {value.length === 0 && (
                    <span className="text-xs text-muted-foreground">尚未配置</span>
                )}
                {value.map((item) => (
                    <Badge
                        key={item}
                        variant="outline"
                        closable={!disabled}
                        className="max-w-full font-mono"
                        onClose={(event) => {
                            event.preventDefault();
                            onChange(value.filter((candidate) => candidate !== item));
                        }}
                    >
                        <span className="truncate">{item}</span>
                    </Badge>
                ))}
            </div>
            <div className="flex gap-2">
                <Input
                    value={draft}
                    list={suggestions.length > 0 ? suggestionListId : undefined}
                    disabled={disabled}
                    placeholder={placeholder}
                    className="h-8 border-0 px-1 shadow-none focus-visible:ring-0"
                    onChange={(event) => setDraft(event.target.value)}
                    onBlur={addDraft}
                    onKeyDown={(event) => {
                        if (event.key !== 'Enter' && event.key !== ',' && event.key !== ' ') return;
                        event.preventDefault();
                        addDraft();
                    }}
                />
                <Button
                    type="button"
                    variant="ghost"
                    size="icon-sm"
                    aria-label="添加配置项"
                    disabled={disabled || !draft.trim()}
                    onMouseDown={(event) => event.preventDefault()}
                    onClick={addDraft}
                >
                    <Plus />
                </Button>
            </div>
            {suggestions.length > 0 && (
                <datalist id={suggestionListId}>
                    {suggestions.map((suggestion) => (
                        <option key={suggestion} value={suggestion} />
                    ))}
                </datalist>
            )}
        </div>
    );
}

const compressionAlgorithms = ['zstd', 'br', 'gzip'] as const;

interface AlgorithmOrderProps {
    value: string[];
    onChange: (value: string[]) => void;
    disabled?: boolean;
    invalid?: boolean;
}

export function AlgorithmOrder({ value, onChange, disabled, invalid }: AlgorithmOrderProps) {
    const move = (index: number, offset: number) => {
        const target = index + offset;
        if (target < 0 || target >= value.length) return;
        const next = [...value];
        [next[index], next[target]] = [next[target], next[index]];
        onChange(next);
    };

    return (
        <div
            className={cn(
                'space-y-2 rounded-lg border bg-muted/20 p-2',
                invalid && 'border-destructive ring-3 ring-destructive/20'
            )}
        >
            {value.map((algorithm, index) => (
                <div
                    key={algorithm}
                    className="flex items-center gap-2 rounded-md border bg-background px-2 py-1.5"
                >
                    <span className="flex size-6 items-center justify-center rounded bg-primary/10 text-xs font-semibold text-primary">
                        {index + 1}
                    </span>
                    <span className="min-w-0 flex-1 font-mono text-sm">{algorithm}</span>
                    <Button
                        type="button"
                        variant="ghost"
                        size="icon-xs"
                        aria-label={`上移 ${algorithm}`}
                        disabled={disabled || index === 0}
                        onClick={() => move(index, -1)}
                    >
                        <ArrowUp />
                    </Button>
                    <Button
                        type="button"
                        variant="ghost"
                        size="icon-xs"
                        aria-label={`下移 ${algorithm}`}
                        disabled={disabled || index === value.length - 1}
                        onClick={() => move(index, 1)}
                    >
                        <ArrowDown />
                    </Button>
                    <Button
                        type="button"
                        variant="ghost"
                        size="icon-xs"
                        aria-label={`移除 ${algorithm}`}
                        disabled={disabled}
                        onClick={() => onChange(value.filter((item) => item !== algorithm))}
                    >
                        <X />
                    </Button>
                </div>
            ))}
            <div className="flex flex-wrap gap-2">
                {compressionAlgorithms
                    .filter((algorithm) => !value.includes(algorithm))
                    .map((algorithm) => (
                        <Button
                            key={algorithm}
                            type="button"
                            variant="outline"
                            size="sm"
                            disabled={disabled}
                            onClick={() => onChange([...value, algorithm])}
                        >
                            <Plus />
                            {algorithm}
                        </Button>
                    ))}
            </div>
        </div>
    );
}

export interface CertificatePickerOption {
    value: string;
    label: string;
    usable: boolean;
}

interface CertificatePickerProps {
    value: string[];
    options: CertificatePickerOption[];
    onChange: (value: string[]) => void;
    onSearchChange?: (value: string) => void;
    loading?: boolean;
    disabled?: boolean;
    invalid?: boolean;
}

export function CertificatePicker({
    value,
    options,
    onChange,
    onSearchChange,
    loading,
    disabled,
    invalid,
}: CertificatePickerProps) {
    const pickerId = useId();
    const [search, setSearch] = useState('');
    const selected = new Set(value);
    const normalizedSearch = search.trim().toLocaleLowerCase();
    const visibleOptions = options.filter(
        (option) =>
            selected.has(option.value) ||
            option.label.toLocaleLowerCase().includes(normalizedSearch)
    );

    const updateSearch = (next: string) => {
        setSearch(next);
        onSearchChange?.(next);
    };

    return (
        <div
            className={cn(
                'overflow-hidden rounded-lg border bg-background',
                invalid && 'border-destructive ring-3 ring-destructive/20',
                disabled && 'opacity-50'
            )}
        >
            <div className="relative border-b p-2">
                <Search className="pointer-events-none absolute left-4 top-1/2 size-4 -translate-y-1/2 text-muted-foreground" />
                <Input
                    value={search}
                    disabled={disabled}
                    placeholder="搜索证书域名"
                    className="pl-9"
                    onChange={(event) => updateSearch(event.target.value)}
                />
            </div>
            <div className="max-h-56 overflow-y-auto p-2">
                {loading && visibleOptions.length === 0 ? (
                    <div className="flex items-center justify-center gap-2 py-8 text-sm text-muted-foreground">
                        <LoaderCircle className="size-4 animate-spin" />
                        正在加载证书
                    </div>
                ) : visibleOptions.length === 0 ? (
                    <p className="py-8 text-center text-sm text-muted-foreground">没有可用证书</p>
                ) : (
                    visibleOptions.map((option, index) => {
                        const checked = selected.has(option.value);
                        const checkboxId = `${pickerId}-${index}`;
                        return (
                            <div
                                key={option.value}
                                className={cn(
                                    'flex items-start gap-3 rounded-md px-2 py-2 hover:bg-accent',
                                    !option.usable && !checked && 'cursor-not-allowed opacity-50'
                                )}
                            >
                                <Checkbox
                                    id={checkboxId}
                                    checked={checked}
                                    disabled={
                                        disabled ||
                                        (!option.usable && !checked) ||
                                        (value.length >= 20 && !checked)
                                    }
                                    onCheckedChange={(nextChecked) => {
                                        if (nextChecked === true)
                                            onChange([...value, option.value]);
                                        else
                                            onChange(value.filter((item) => item !== option.value));
                                    }}
                                />
                                <label
                                    htmlFor={checkboxId}
                                    className="min-w-0 flex-1 cursor-pointer text-sm"
                                >
                                    <span className="block break-words">{option.label}</span>
                                    {!option.usable && (
                                        <span className="mt-0.5 block text-xs text-destructive">
                                            已不可用，请移除
                                        </span>
                                    )}
                                </label>
                                {checked && <Check className="size-4 shrink-0 text-primary" />}
                            </div>
                        );
                    })
                )}
            </div>
        </div>
    );
}
