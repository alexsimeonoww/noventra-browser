<script lang="ts">
    import { onMount } from 'svelte';
    import { sendWithPromise } from '../lib/cr';
    import { installExtension, WebstoreInstallErrorCode, type ExtensionStatus } from '../lib/browser/extensions';
    import { passwordManagers } from '../lib/password-managers';
    import { passwordManagerIcons } from '../lib/nonfree-icons';
    import { platform } from '../lib/platform';
    import IconKey from '../icons/tabler/IconKey.svelte';

    let {
        extensions = $bindable<ExtensionStatus>({}),
        installing = $bindable(false),
        refreshExtensions,
    }: {
        extensions?: ExtensionStatus;
        installing?: boolean;
        refreshExtensions: () => Promise<void>;
    } = $props();

    const managers = Object.entries(passwordManagers);
    const descriptions: Record<string, string> = {
        protonPass: 'Passwords and email aliases',
        bitwarden: 'Open-source password vault',
        onePassword: 'Account or desktop app',
        keePassXC: 'Requires the KeePassXC desktop app',
        dashlane: 'Password manager',
        icloudPasswords: 'Requires macOS Sonoma+ or iCloud for Windows',
    };
    let loading = $state(true);
    let checkFailed = $state(false);
    let workingId = $state('');
    let messages = $state<Record<string, string>>({});
    let brokenIcons = $state<Record<string, boolean>>({});

    async function refresh() {
        loading = true;
        checkFailed = false;
        try {
            await refreshExtensions();
        } catch {
            checkFailed = true;
        } finally {
            loading = false;
        }
    }

    async function install(id: string) {
        const info = passwordManagers[id];
        if (!info || installing || loading || checkFailed || extensions[info.extensionId] ||
            (info.platforms && !info.platforms.includes(platform.name))) return;

        installing = true;
        workingId = info.extensionId;
        messages[id] = '';
        try {
            if (!(info.extensionId in extensions)) {
                // This opt-in applies only to extension downloads. Service
                // consent, browser updates and other service settings stay put.
                await sendWithPromise('setPref', 'extensions.chrome_web_store', true);
                const prefs = await sendWithPromise<Record<string, unknown>>('getPrefs');
                if (prefs['extensions.chrome_web_store'] !== true) {
                    throw new Error('Downloads were not enabled');
                }
            }
            const result = await installExtension(info.extensionId);
            await refresh();
            if (extensions[info.extensionId]) {
                messages[id] = 'Ready. Open the extension to sign in.';
            } else if (!result.success && result.code === WebstoreInstallErrorCode.UserCancelled) {
                messages[id] = 'Canceled. Try again whenever you’re ready.';
            } else if (!result.success && (result.code === WebstoreInstallErrorCode.BlockedByPolicy ||
                       result.code === WebstoreInstallErrorCode.NotPermitted)) {
                messages[id] = 'Blocked by browser or organization settings.';
            } else {
                messages[id] = 'The extension isn’t enabled. Try again.';
            }
        } catch {
            messages[id] = 'Couldn’t install. Check your connection and Web Store downloads in Settings.';
        } finally {
            installing = false;
            workingId = '';
        }
    }

    onMount(() => {
        void refresh();
    });
</script>

<div class="managers" aria-busy={loading || installing}>
    {#if checkFailed}
        <div class="check-error" role="status">Couldn’t check installed extensions.
            <button id="retry-managers" onclick={refresh} disabled={installing}>Try again</button>
        </div>
    {/if}
    {#each managers as [id, info]}
        {@const supported = !info.platforms || info.platforms.includes(platform.name)}
        {@const enabled = extensions[info.extensionId] === true}
        {@const disabled = extensions[info.extensionId] === false}
        {@const icon = passwordManagerIcons[id.toLowerCase()]}
        <article class="manager" data-manager={id}>
            <div class="manager-top">
                <span class="manager-icon" aria-hidden="true">
                    {#if icon && !brokenIcons[id]}
                        <img src={icon} width="32" height="32" alt="" onerror={() => brokenIcons[id] = true} />
                    {:else}
                        <IconKey />
                    {/if}
                </span>
                <div class="manager-name">
                    <h3>{info.title}</h3>
                    <p>{supported ? descriptions[id] : 'Requires macOS or Windows'}</p>
                </div>
                {#if supported}
                    <button id={`install-${id}`} class:installed={enabled} class="install"
                        disabled={loading || checkFailed || installing || enabled}
                        onclick={() => install(id)} aria-label={`${disabled ? 'Enable' : 'Install'} ${info.title}`}>
                        {workingId === info.extensionId ? 'Installing…' :
                            enabled ? 'Installed ✓' : loading ? 'Checking…' : disabled ? 'Enable' : 'Install'}
                    </button>
                {:else}
                    <a id={`view-${id}`} class="view-extension" href={`https://chromewebstore.google.com/detail/${info.extensionId}`} target="_blank" rel="noopener noreferrer" aria-label={`View ${info.title} extension`}>View extension</a>
                {/if}
            </div>
            <details class="manager-details">
                <summary>Details</summary>
                <div class="manager-links">
                    {#if info.importGuide}<a href={info.importGuide} target="_blank" rel="noopener noreferrer">Import guide</a>{/if}
                    {#if info.setupGuide}<a href={info.setupGuide} target="_blank" rel="noopener noreferrer">{id === 'onePassword' ? 'Connect the desktop app' : 'Setup guide'}</a>{/if}
                    <a href={info.privacyPolicy} target="_blank" rel="noopener noreferrer">Privacy</a>
                </div>
            </details>
            {#if messages[id]}<p class="manager-message" role="status" aria-live="polite">{messages[id]}</p>{/if}
        </article>
    {/each}
</div>
<details class="download-details">
    <summary>Extension download details</summary>
    <p>Install enables Chrome Web Store downloads and shows the extension’s permission prompt. Manage downloads in <a href="chrome://settings/services" target="_blank" rel="noopener noreferrer">Noventra services</a>.</p>
    <p>Sign in inside the extension. Your vault and master password stay outside setup.</p>
</details>

<style>
    .managers { display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:10px; }
    .manager { border:1px solid var(--line);border-radius:12px;padding:14px;min-width:0;align-self:start; }
    .manager-top { display:flex;align-items:center;gap:10px; }
    .manager-icon { display:grid;place-items:center;width:32px;height:32px;flex-shrink:0; }
    .manager-icon img { width:32px;height:32px;object-fit:contain; }
    .manager-icon :global(svg) { width:25px;height:25px; }
    .manager-name { min-width:0;flex:1; }
    h3 { margin:0;font-size:13px;font-weight:600;overflow-wrap:anywhere; }
    .manager-name p { margin:4px 0 0;color:var(--muted);font-size:10px;line-height:1.4; }
    .managers button,.view-extension { cursor:pointer;font:inherit;min-height:36px;border-radius:8px;border:1px solid var(--line);padding:7px 10px;background:var(--ink);color:var(--surface);font-size:11px;white-space:nowrap;flex-shrink:0; }
    .managers button:disabled { cursor:default;opacity:.5; }
    .managers button.installed { background:var(--soft);color:var(--ink);opacity:1; }
    .view-extension { display:flex;align-items:center;text-decoration:none;color:var(--ink);background:var(--soft);white-space:normal;text-align:center;max-width:84px;line-height:1.3; }
    .managers button:focus-visible,a:focus-visible,summary:focus-visible { outline:2px solid var(--ink);outline-offset:4px; }
    .manager-details { margin-top:11px;color:var(--muted);font-size:10px;line-height:1.5; }
    summary { cursor:pointer;width:fit-content; }
    .manager-links { display:flex;gap:6px 14px;flex-wrap:wrap;margin-top:7px; }
    .manager-message { margin:10px 0 0;color:var(--ink);font-size:11px;line-height:1.5;overflow-wrap:anywhere; }
    a { color:inherit;text-underline-offset:3px; }
    .download-details { margin-top:15px;color:var(--muted);font-size:11px;line-height:1.5; }
    .download-details p { margin:8px 0 0; }
    .check-error { grid-column:1/-1;font-size:12px;color:var(--muted);display:flex;align-items:center;gap:8px;flex-wrap:wrap; }
    .check-error button { background:var(--soft);color:var(--ink); }
    @media(max-width:620px) { .managers { grid-template-columns:1fr; }.manager { padding:12px; }.manager-name p { font-size:11px; } }
    @media(max-width:360px) { .manager-top { gap:8px; }.manager-icon { width:28px; }.manager-icon img { width:28px;height:28px; }.managers button,.view-extension { padding:7px 8px;font-size:10px; }.manager-name p { font-size:10px; } }
</style>
