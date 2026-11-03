<script lang="ts">
    import { onMount, tick } from 'svelte';
    import NoventraLogo from './components/NoventraLogo.svelte';
    import { IconWorld } from '@imput/helium-prism';
    import IconKey from './icons/tabler/IconKey.svelte';
    import IconTransferIn from './icons/tabler/IconTransferIn.svelte';
    import { browserIcons, searchEngineIcons } from './lib/nonfree-icons';
    import NoventraPasswordManagers from './components/NoventraPasswordManagers.svelte';
    import { passwordManagers } from './lib/password-managers';
    import { getBoolean } from './lib/strings';
    import { SearchEnginesBrowserProxyImpl, ChoiceMadeLocation, type SearchEngine, type SearchEnginesInfo } from './lib/cr/search_engines_browser_proxy';
    import { DefaultBrowserBrowserProxyImpl, type DefaultBrowserInfo } from './lib/cr/default_browser_browser_proxy';
    import type { ExtensionStatus } from './lib/browser/extensions';
    import { addWebUiListener, removeWebUiListener, sendWithPromise } from './lib/cr';
    import { ImportDataBrowserProxyImpl, ImportDataStatus, type BrowserProfile } from './lib/cr/import_data_browser_proxy';

    const steps = ['Welcome', 'Data', 'Password manager', 'Search engine', 'Ready'];
    const importer = ImportDataBrowserProxyImpl.getInstance();
    const showMacImportNotice = getBoolean('showMacImportPermissionNotice');
    const canImportBrowserPasswords = getBoolean('canImportBrowserPasswords');
    const searchProxy = SearchEnginesBrowserProxyImpl.getInstance();
    const defaultProxy = DefaultBrowserBrowserProxyImpl.getInstance();
    const choiceKey = 'noventra.setup.imports.v2';
    const legacyChoiceKey = 'noventra.setup.imports.v1';
    type DataKind = 'bookmarks' | 'history' | 'passwords' | 'cookies';
    type Choices = Record<DataKind, boolean>;
    type Completed = Record<number, DataKind[]>;
    const kinds: DataKind[] = ['bookmarks', 'history', 'passwords', 'cookies'];
    const kindLabels: Record<DataKind, string> = { bookmarks: 'Bookmarks', history: 'History', passwords: 'Passwords', cookies: 'Cookies and signed-in sessions' };
    const validId = (value: unknown): value is number => typeof value === 'number' && Number.isSafeInteger(value) && value >= 0;
    const validCount = (value: unknown): number => typeof value === 'number' && Number.isSafeInteger(value) && value >= 0 ? value : 0;

    function restoreChoices(): { id?: number; data: Choices; completed: Completed; bookmarks: number; history: number; passwords: number; interrupted: boolean; csv: boolean; restorable: boolean; version: number; searchId?: number; searchChanged: boolean; makeDefault: boolean } {
        const empty = { data: { bookmarks: true, history: true, passwords: true, cookies: false }, completed: {} as Completed, bookmarks: 0, history: 0, passwords: 0, interrupted: false, csv: false, restorable: false, version: 0, searchId: undefined as number | undefined, searchChanged: false, makeDefault: false };
        try {
            const value = JSON.parse(sessionStorage.getItem(choiceKey) ?? sessionStorage.getItem(legacyChoiceKey) ?? 'null');
            if (!value || ![1, 2].includes(value.version)) return empty;
            const completed: Completed = {};
            if (value.completed && typeof value.completed === 'object') {
                for (const [id, imported] of Object.entries(value.completed)) {
                    if (validId(Number(id)) && Array.isArray(imported)) completed[Number(id)] = kinds.filter(kind => imported.includes(kind));
                }
            }
            return { id: validId(value.id) ? value.id : undefined,
                data: Object.fromEntries(kinds.map(kind => [kind, kind !== 'cookies' && value.data?.[kind] === true])) as Choices,
                completed, bookmarks: validCount(value.bookmarks), history: validCount(value.history), passwords: validCount(value.passwords),
                interrupted: value.inFlight === true || value.interrupted === true, csv: value.csv === true, restorable: true,
                version: value.version, searchId: value.version === 2 && validId(value.searchId) ? value.searchId : undefined,
                searchChanged: value.version === 2 && value.searchChanged === true && validId(value.searchId),
                makeDefault: value.version === 2 && value.makeDefault === true };
        } catch { return empty; }
    }

    const restored = restoreChoices();
    // Version 1 used four steps; its Ready index moves past the new search step.
    const storedStep = restored.restorable ? Number(sessionStorage.getItem('noventra.setup.step') ?? 0) : 0;
    const savedStep = restored.version === 1 && storedStep === 3 ? 4 : storedStep;
    if (savedStep !== storedStep) sessionStorage.setItem('noventra.setup.step', String(savedStep));
    if (!restored.restorable) sessionStorage.removeItem('noventra.setup.step');
    let step = $state(Number.isInteger(savedStep) && savedStep >= 0 && savedStep < steps.length ? savedStep : 0);
    let profiles = $state<BrowserProfile[]>([]);
    let discovery = $state<'initial' | 'loading' | 'loaded' | 'error'>('initial');
    let selectedId = $state<number | undefined>(restored.id);
    let selectedData = $state<Choices>(restored.data);
    let importedProfileData = $state<Completed>(restored.completed);
    let bookmarkImports = $state(restored.bookmarks);
    let historyImports = $state(restored.history);
    let passwordImports = $state(restored.passwords);
    let cookieImports = $state(0);
    let cookieSkipped = $state(0);
    let cookieResult = $state('');
    let passwordOpened = $state(restored.csv);
    let interruptedImport = $state(restored.interrupted);
    let importInFlight = $state(false);
    let status = $state<'idle' | 'choosing' | 'importing' | 'success' | 'error'>('idle');
    let finishing = $state(false);
    let saving = $state(false);
    let saveError = $state(false);
    let importError = $state('');
    let importProgress = $state('');
    let fileImportIssue = $state(false);
    let title = $state<HTMLHeadingElement>();
    let managerExtensions = $state<ExtensionStatus>({});
    let managerInstalling = $state(false);
    let managerLoading = $state(true);
    let managerCheckFailed = $state(false);
    let defaultsLoading = $state(false);
    let defaultsLoaded = false;
    let searchEngines = $state<SearchEngine[]>([]);
    let selectedSearchEngineId = $state<number | undefined>(restored.searchId);
    let searchChanged = $state(restored.searchChanged);
    let searchLoadError = $state(false);
    let defaultBrowserInfo = $state<DefaultBrowserInfo>();
    let defaultLoadError = $state(false);
    let makeDefault = $state(restored.makeDefault);
    let defaultRequestSent = false;
    let defaultsApplyFailed = $state(false);
    let discoveryPromise: Promise<void> | undefined;
    let resolveImport: ((success: boolean) => void) | undefined;
    let importingKinds: DataKind[] = [];
    let importingProfileId: number | undefined;
    const enabledManagers = $derived(Object.values(passwordManagers).filter(info => managerExtensions[info.extensionId]).map(info => info.title));
    const busy = $derived(finishing || status === 'importing' || saving || managerInstalling);
    const discoveryBusy = $derived(discovery === 'loading');
    const chosenProfile = $derived(profiles.find(p => p.id === selectedId));
    const stagedKinds = $derived.by(() => {
        const id = selectedId;
        return id === undefined ? [] : kinds.filter(kind => selectedData[kind] && !importedProfileData[id]?.includes(kind));
    });
    const stagedDescription = $derived(stagedKinds.map(kind => kindLabels[kind].toLowerCase()).join(', '));
    const searchManaged = $derived(searchEngines.some(engine => engine.default && engine.isManaged));
    const sourceLabel = $derived(chosenProfile ? `${chosenProfile.name}${chosenProfile.profileName ? ` · ${chosenProfile.profileName}` : ''}` : 'Selected browser');

    $effect(() => {
        try {
            sessionStorage.setItem(choiceKey, JSON.stringify({ version: 2, id: selectedId, data: selectedData,
                completed: importedProfileData, bookmarks: bookmarkImports, history: historyImports, passwords: passwordImports,
                inFlight: importInFlight, interrupted: interruptedImport, csv: passwordOpened,
                searchId: selectedSearchEngineId, searchChanged, makeDefault }));
            sessionStorage.removeItem(legacyChoiceKey);
        } catch { /* Storage restrictions must not prevent an explicitly requested import. */ }
    });

    function availableData(profile: BrowserProfile): DataKind[] {
        return [...(profile.favorites ? ['bookmarks' as const] : []), ...(profile.history ? ['history' as const] : []),
            ...(canImportBrowserPasswords && profile.passwords ? ['passwords' as const] : []),
            ...(profile.cookies ? ['cookies' as const] : [])];
    }

    function dataComplete(profile: BrowserProfile) {
        const available = availableData(profile).filter(kind => kind !== 'cookies' || importedProfileData[profile.id]?.includes(kind));
        return available.length > 0 && available.every(kind => importedProfileData[profile.id]?.includes(kind));
    }

    function selectData(profile: BrowserProfile) {
        status = 'idle';
        importError = '';
        selectedData = Object.fromEntries(kinds.map(kind => [kind, kind !== 'cookies' && availableData(profile).includes(kind) && !importedProfileData[profile.id]?.includes(kind)])) as Choices;
    }

    async function refreshManagerExtensions() {
        managerLoading = true;
        try {
            managerExtensions = await sendWithPromise<ExtensionStatus>('getExtensions');
            managerCheckFailed = false;
        } catch (error) {
            managerCheckFailed = true;
            throw error;
        } finally { managerLoading = false; }
    }

    async function go(next: number) {
        if (busy) return;
        step = Math.max(0, Math.min(steps.length - 1, next));
        status = 'idle';
        sessionStorage.setItem('noventra.setup.step', String(step));
        await tick();
        title?.focus();
        if (step === 1 && discovery === 'initial') await refreshBrowserProfiles();
        if (step >= 3 && !defaultsLoaded) void loadDefaults();
    }

    function scanBrowserProfiles(): Promise<void> {
        if (discoveryPromise) return discoveryPromise;
        discovery = 'loading';
        discoveryPromise = importer.initializeImportDialog().then(list => {
            // The native API appends its separate bookmarks-file entry last.
            profiles = list.slice(0, -1);
            discovery = 'loaded';
        }).catch(() => { discovery = 'error'; }).finally(() => { discoveryPromise = undefined; });
        return discoveryPromise;
    }

    function refreshBrowserProfiles() {
        if (busy) return;
        status = 'idle';
        return scanBrowserProfiles();
    }

    function eligibleEngines(info: SearchEnginesInfo): SearchEngine[] {
        const seen = new Set<number>();
        return Object.values(info).flat().filter(engine => {
            if (seen.has(engine.id) || engine.isStarterPack || engine.isOmniboxExtension || (!engine.default && !engine.canBeDefault)) return false;
            seen.add(engine.id);
            return true;
        });
    }

    function searchIcon(engine: SearchEngine): string | undefined {
        if (!engine.isPrepopulated) return undefined;
        const name = engine.name.toLowerCase().replace(/[^a-z]/g, '');
        const known = Object.keys(searchEngineIcons).find(key => name.includes(key));
        return known ? searchEngineIcons[known] : undefined;
    }

    function keepCurrentSearch() {
        searchChanged = false;
        selectedSearchEngineId = searchEngines.find(engine => engine.default)?.id;
        importError = '';
        defaultsApplyFailed = false;
    }

    async function loadDefaults() {
        if (defaultsLoading) return;
        defaultsLoading = true;
        const [search, browser] = await Promise.allSettled([searchProxy.getSearchEnginesList(), defaultProxy.requestDefaultBrowserState()]);
        if (search.status === 'fulfilled') {
            searchEngines = eligibleEngines(search.value);
            if (!searchChanged) selectedSearchEngineId = searchEngines.find(engine => engine.default)?.id;
            searchLoadError = false;
        } else searchLoadError = true;
        if (browser.status === 'fulfilled') { defaultBrowserInfo = browser.value; defaultLoadError = false; }
        else defaultLoadError = true;
        defaultsLoading = false;
        defaultsLoaded = true;
    }

    async function applyDefaults(): Promise<boolean> {
        defaultsApplyFailed = false;
        try {
            if (searchChanged && selectedSearchEngineId !== undefined) {
                const fresh = eligibleEngines(await searchProxy.getSearchEnginesList());
                searchEngines = fresh;
                const selected = fresh.find(engine => engine.id === selectedSearchEngineId);
                if (!selected || fresh.some(engine => engine.default && engine.isManaged) || (!selected.default && (!selected.canBeDefault || selected.isManaged))) {
                    defaultsApplyFailed = true;
                    importError = 'Your search choice is unavailable. Review Search engine and try again.';
                    return false;
                }
                if (!selected.default) searchProxy.setDefaultSearchEngine(selected.id, ChoiceMadeLocation.OTHER, null);
                const verified = eligibleEngines(await searchProxy.getSearchEnginesList());
                searchEngines = verified;
                if (!verified.some(engine => engine.id === selectedSearchEngineId && engine.default)) {
                    defaultsApplyFailed = true;
                    importError = 'The search engine could not be changed. Review Search engine and try again.';
                    return false;
                }
                searchChanged = false;
            }
            if (makeDefault && !defaultRequestSent) {
                const fresh = await defaultProxy.requestDefaultBrowserState();
                defaultBrowserInfo = fresh;
                if (!fresh.isDefault) {
                    if (!fresh.canBeDefault || fresh.isDisabledByPolicy || fresh.isUnknownError) {
                        defaultsApplyFailed = true;
                        importError = 'Noventra cannot become the default browser right now. Review Browsing defaults and try again.';
                        return false;
                    }
                    defaultProxy.setAsDefaultBrowser(false);
                    defaultRequestSent = true;
                }
            }
            return true;
        } catch {
            defaultsApplyFailed = true;
            importError = 'Browsing defaults could not be applied. Review your choices and try again.';
            return false;
        }
    }

    function keepCurrentDefaults() {
        searchChanged = false;
        selectedSearchEngineId = searchEngines.find(engine => engine.default)?.id;
        makeDefault = false;
        if (defaultsApplyFailed) importError = '';
        defaultsApplyFailed = false;
    }

    async function saveCompletion() {
        saving = true;
        saveError = false;
        try {
            await sendWithPromise('setPref', 'completed_onboarding', true);
            sessionStorage.removeItem('noventra.setup.step');
            sessionStorage.removeItem(choiceKey);
            sessionStorage.removeItem(legacyChoiceKey);
            location.assign('chrome://newtab/');
        } catch { saveError = true; }
        finally { saving = false; }
    }

    async function skipSetup() {
        if (busy) return;
        await saveCompletion();
    }

    async function importJob(profile: BrowserProfile, selected: DataKind[]) {
        importingProfileId = profile.id;
        importingKinds = selected;
        if (selected.includes('cookies')) {
            cookieImports = cookieSkipped = 0;
            cookieResult = '';
        }
        importProgress = `Importing ${selected.map(kind => kindLabels[kind].toLowerCase()).join(' and ')} from ${profile.name}…`;
        status = 'importing';
        importInFlight = true;
        const result = new Promise<boolean>(resolve => { resolveImport = resolve; });
        try {
            const types = {
                import_dialog_bookmarks: selected.includes('bookmarks'), import_dialog_history: selected.includes('history'),
                import_dialog_saved_passwords: selected.includes('passwords'), import_dialog_search_engine: false,
                import_dialog_cookies: selected.includes('cookies'),
                import_dialog_autofill_form_data: false, import_dialog_extensions: false,
            };
            importer.importData(profile.index, types);
        } catch { status = 'error'; resolveImport?.(false); }
        const success = await result;
        resolveImport = undefined;
        importInFlight = false;
        return success;
    }

    async function finish() {
        if (busy) return;
        finishing = true;
        saveError = false;
        importError = '';
        try {
            if (stagedKinds.length) {
                importProgress = 'Checking the selected browser…';
                // Resolve the stable ID again immediately before importing. A
                // disappeared profile or changed capability is never replaced
                // with another profile at the same old discovery index.
                await scanBrowserProfiles();
                const profile = profiles.find(p => p.id === selectedId);
                if (discovery !== 'loaded' || !profile || !stagedKinds.every(kind => availableData(profile).includes(kind))) {
                    importError = 'The selected browser data is unavailable. Go back to Data, refresh and review your choices.';
                    return;
                }
                const data = stagedKinds.filter(kind => kind !== 'passwords');
                const jobs: DataKind[][] = [...(data.length ? [data] : []), ...(stagedKinds.includes('passwords') ? [['passwords' as const]] : [])];
                for (const job of jobs) {
                    if (!await importJob(profile, job)) {
                        importError = job.includes('passwords')
                            ? 'Passwords couldn’t be copied. Close the source browser and unlock its credential store, then retry. Completed imports are kept.'
                            : job.includes('cookies') ? 'Cookies could not be copied. Close the source browser and unlock its credential store, then retry. Some sites may require a new sign-in.'
                            : 'Browser data couldn’t be copied. Some data may already have been copied. Close the source browser, then retry.';
                        return;
                    }
                }
                interruptedImport = false;
            }
            if (!await applyDefaults()) return;
            await saveCompletion();
        } finally {
            finishing = false;
            importProgress = '';
        }
    }

    function importFile() {
        if (busy || discoveryBusy) return;
        selectedId = undefined;
        importingProfileId = undefined;
        importingKinds = ['bookmarks'];
        status = 'choosing';
        importer.importFromBookmarksFile();
    }

    function openPasswords() {
        if (busy) return;
        selectedData.passwords = false;
        window.open('chrome://password-manager/settings?import=true', '_blank', 'noopener');
        passwordOpened = true;
    }

    function openMacImportSettings() { if (showMacImportNotice) chrome.send('openFilesAndFoldersSettings'); }
    const browserLogoNames: Record<string, string> = {
        'Google Chrome': 'chrome', 'Google Chrome Beta': 'chrome_beta', 'Google Chrome Canary': 'chrome_canary', 'Google Chrome Dev': 'chrome_dev',
        'Microsoft Edge': 'edge', 'Mozilla Firefox': 'firefox', 'NAVER Whale': 'whale', 'Perplexity Comet': 'comet',
    };
    const browserLogo = (name: string) => browserIcons[browserLogoNames[name] ?? name.toLowerCase()];

    onMount(() => {
        document.title = 'Set up Noventra';
        if (step === 1 || selectedId !== undefined) void scanBrowserProfiles();
        if (step >= 3) void loadDefaults();
        const extensionListener = addWebUiListener('extension-state-changed', (diff: ExtensionStatus) => { managerExtensions = { ...managerExtensions, ...diff }; });
        void refreshManagerExtensions().catch(() => {});
        const cookieListener = addWebUiListener('import-cookies-result', (result: { imported: number; skipped: number }) => {
            if (status !== 'importing' || !importingKinds.includes('cookies') ||
                !Number.isSafeInteger(result.imported) || result.imported < 0 ||
                !Number.isSafeInteger(result.skipped) || result.skipped < 0) return;
            cookieImports += result.imported;
            cookieSkipped += result.skipped;
            cookieResult = `${cookieImports} cookies imported. ${cookieSkipped} skipped. Some sites may ask you to sign in again.`;
        });
        const listener = addWebUiListener('import-data-status-changed', (value: ImportDataStatus) => {
            if (value === ImportDataStatus.IN_PROGRESS) status = 'importing';
            if (value === ImportDataStatus.SUCCEEDED && status === 'importing') {
                status = 'success';
                if (importingKinds.includes('bookmarks')) bookmarkImports += 1;
                if (importingKinds.includes('history')) historyImports += 1;
                if (importingKinds.includes('passwords')) passwordImports += 1;
                if (importingProfileId !== undefined) importedProfileData = { ...importedProfileData,
                    [importingProfileId]: [...new Set([...(importedProfileData[importingProfileId] ?? []), ...importingKinds])] };
                else fileImportIssue = false;
                resolveImport?.(true);
            }
            if (value === ImportDataStatus.FAILED && (status === 'importing' || status === 'choosing')) {
                status = 'error';
                if (importingProfileId === undefined) fileImportIssue = true;
                resolveImport?.(false);
            }
        });
        const searchListener = addWebUiListener('search-engines-changed', (info: SearchEnginesInfo) => {
            searchEngines = eligibleEngines(info);
            if (!searchChanged) selectedSearchEngineId = searchEngines.find(engine => engine.default)?.id;
            searchLoadError = false;
        });
        const defaultListener = addWebUiListener('browser-default-state-changed', (state: DefaultBrowserInfo) => { defaultBrowserInfo = state; });
        return () => { removeWebUiListener(listener); removeWebUiListener(extensionListener); removeWebUiListener(defaultListener); removeWebUiListener(searchListener); removeWebUiListener(cookieListener); };
    });
</script>

<main id="noventra-onboarding">
    <header class="brand-bar">
        <div class="brand"><NoventraLogo height="26px" /><span>Noventra</span></div>
        <button id="skip-setup" class="quiet" disabled={busy} onclick={skipSetup}>Skip setup</button>
    </header>
    <div class="setup-shell">
        <nav class="setup-progress" aria-label="Setup progress">
            <ol class="steps">
                {#each steps as label, index}
                    <li class:active={step === index} class:complete={step > index} aria-current={step === index ? 'step' : undefined}>
                        <span class="step-number" aria-hidden="true">{step > index ? '✓' : index + 1}</span><span>{label}</span>
                    </li>
                {/each}
            </ol>
        </nav>
        <section class="panel" data-discovery={discovery} aria-labelledby="step-title" aria-busy={busy || (step === 1 && discoveryBusy)}>
            <div class="panel-content">
                <div class="step-count">STEP {step + 1} OF {steps.length}<span>OPTIONAL</span></div>
                {#if step === 0}
                    <div class="welcome-logo" aria-hidden="true"><NoventraLogo height="56px" /></div>
                    <h1 id="step-title" tabindex="-1" bind:this={title}>Make yourself at home.</h1>
                    <p class="intro">Bring your browser data and password manager.</p>
                    <a class="transfer-link" href="chrome://settings/dataDevices" target="_blank" rel="noopener noreferrer">Moving from another PC? Restore or connect a device.</a>
                {:else if step === 1}
                    <h1 id="step-title" tabindex="-1" bind:this={title}>Bring your browsing data.</h1>
                    <p class="intro">Choose what to bring. We’ll copy it when you finish setup.</p>
                    <div class="source-heading"><strong>On this device</strong><span>{discovery === 'initial' ? 'Ready to scan' : discovery === 'loading' ? 'Looking…' : discovery === 'error' ? 'Check needed' : `${profiles.length} profiles`}</span></div>
                    {#if discovery === 'initial'}
                        <div class="empty"><strong>Find browsers on this device.</strong><p>Choose what to import after scanning.</p></div>
                    {:else if discovery === 'loading'}
                        <div class="empty" role="status"><span class="spinner" aria-hidden="true"></span>Looking for browser profiles…</div>
                    {:else if profiles.length === 0}
                        <div class="empty" role={discovery === 'error' ? 'alert' : undefined}><strong>{discovery === 'error' ? 'We couldn’t check your browser profiles.' : 'No browser profiles found.'}</strong><p>Refresh or use a file below.</p></div>
                    {:else}
                        <fieldset class="profile-list" disabled={busy || discoveryBusy || discovery === 'error'}>
                            <legend class="sr-only">Choose a browser profile</legend>
                            {#each profiles as profile (profile.id)}
                                <label class="profile-card" class:selected={selectedId === profile.id} class:imported={dataComplete(profile)}>
                                    <input type="radio" name="source-profile" value={profile.id} bind:group={selectedId} disabled={busy || discoveryBusy || discovery === 'error' || !availableData(profile).length || dataComplete(profile)} onchange={() => selectData(profile)} />
                                    <span class="source-icon" aria-hidden="true">{#if browserLogo(profile.name)}<img src={browserLogo(profile.name)} alt="" />{:else}<IconWorld />{/if}</span>
                                    <span class="source-name"><strong>{profile.name}</strong><span>{profile.profileName || 'Default profile'}</span></span>
                                    <span class="source-kind">{dataComplete(profile) ? 'Imported ✓' : availableData(profile).length ? availableData(profile).map(kind => kindLabels[kind]).join(' · ') : 'No compatible data'}</span>
                                </label>
                            {/each}
                        </fieldset>
                        {#if chosenProfile?.cookies}<p class="privacy-hint">Cookie import is optional. Cookies already in this Space are kept. Some sites may ask you to sign in again.</p>{/if}
                        {#if cookieResult}<p id="importCookiesResult" class="import-message" role="status">{cookieResult}</p>{/if}
                    {/if}
                    <div class="discovery-actions">{#if selectedId !== undefined}<button id="clear-import-selection" class="quiet" disabled={busy} onclick={() => { selectedId = undefined; importError = ''; }}>Clear selection</button>{/if}<button id={discovery === 'initial' ? 'find-import-profiles' : 'refresh-import-profiles'} class="quiet" disabled={busy || discoveryBusy} onclick={refreshBrowserProfiles}>{discovery === 'initial' ? 'Find browsers' : discovery === 'loading' ? 'Looking…' : discovery === 'error' ? 'Try again' : 'Refresh browsers'}</button></div>
                    {#if discovery === 'error' && profiles.length > 0}<p class="error" role="alert">Refresh before choosing a profile, or use a file.</p>{/if}
                    {#if showMacImportNotice}<aside class="mac-import-notice" aria-label="Browser import access"><p>Missing a browser? Allow access in macOS, then refresh.</p><button id="mac-import-settings" class="quiet" disabled={busy} onclick={openMacImportSettings}>Open Files and Folders settings</button></aside>{/if}
                    {#if chosenProfile}
                        <fieldset class="data-types" disabled={busy || discoveryBusy || discovery === 'error'}>
                            <legend>Bring from {chosenProfile.name}</legend>
                            {#each kinds as kind}
                                <label><input id={`import-data-${kind}`} type="checkbox" checked={selectedData[kind] && availableData(chosenProfile).includes(kind) && !importedProfileData[chosenProfile.id]?.includes(kind)} disabled={!availableData(chosenProfile).includes(kind) || importedProfileData[chosenProfile.id]?.includes(kind)} onchange={event => { selectedData[kind] = event.currentTarget.checked; importError = ''; }} />{kindLabels[kind]}{#if importedProfileData[chosenProfile.id]?.includes(kind)}<span>Copied</span>{:else if !availableData(chosenProfile).includes(kind)}<span>{kind === 'passwords' ? 'Use CSV' : 'Unavailable'}</span>{/if}</label>
                            {/each}
                        </fieldset>
                        {#if stagedKinds.length}<p id="queued-import-note" class="privacy-hint">Selected for the end of setup: {stagedDescription}.</p>{/if}
                    {/if}
                    <details class="import-details file-fallbacks"><summary>Import from a file instead</summary>
                        <p>File imports open now. Return here afterwards to finish setup.</p>
                        <button id="bookmark-file" class="file-card" onclick={importFile} disabled={busy || discoveryBusy}><span class="source-icon" aria-hidden="true"><IconTransferIn /></span><span class="source-name"><strong>Choose a bookmarks file</strong><span>HTML from your previous browser</span></span><span aria-hidden="true">↗</span></button>
                        <button id="password-import" class="file-card" disabled={busy} onclick={openPasswords}><span class="source-icon" aria-hidden="true"><IconKey /></span><span class="source-name"><strong>{passwordOpened ? 'Open password import again' : 'Import passwords from CSV'}</strong><span>Opens the password manager</span></span><span aria-hidden="true">↗</span></button>
                        <p>Existing passwords are kept. For CSV, export from your previous browser, then select it in Noventra’s password manager. Remove the readable export afterwards.</p>
                    </details>
                    <div class="import-message" role="status" aria-live="polite">{#if status === 'success'}<span class="success">✓ Bookmark import finished.</span>{:else if status === 'error'}<span class="error">Import didn’t finish. Some data may already have been copied. Close the source browser and try again.</span>{:else if status === 'choosing'}Choose an HTML file, or cancel to continue.{:else if status === 'importing'}Copying the selected file. Keep this page open.{/if}</div>
                    {#if passwordOpened}<p class="import-message" role="status">Password import opened in another tab. Check the results there.</p>{/if}
                {:else if step === 2}
                    <h1 id="step-title" tabindex="-1" bind:this={title}>Keep your password manager.</h1>
                    <p class="intro">Add an extension, then sign in as usual.</p>
                    <NoventraPasswordManagers bind:extensions={managerExtensions} bind:installing={managerInstalling} refreshExtensions={refreshManagerExtensions} />
                {:else if step === 3}
                    <h1 id="step-title" tabindex="-1" bind:this={title}>Choose your search engine.</h1>
                    <p class="intro">Search from the address bar. Your choice takes effect when you finish setup.</p>
                    {#if defaultsLoading}<p class="import-message" role="status">Loading search engines…</p>{/if}
                    {#if searchManaged}<p class="privacy-hint">Your search engine is managed. Your current choice will be kept.</p>{/if}
                    <fieldset id="setup-search-engine" class="search-engines" disabled={busy || defaultsLoading || searchLoadError || searchManaged || !searchEngines.length}>
                        <legend class="sr-only">Default search engine</legend>
                        {#each searchEngines as engine (engine.id)}
                            <label class="search-engine-card" class:selected={selectedSearchEngineId === engine.id} data-search-id={engine.id}>
                                <input type="radio" name="setup-search-engine" value={engine.id} checked={selectedSearchEngineId === engine.id} disabled={!engine.default && (!engine.canBeDefault || engine.isManaged)} onchange={() => { selectedSearchEngineId = engine.id; searchChanged = true; importError = ''; }} />
                                <span class="source-icon" aria-hidden="true">{#if searchIcon(engine)}<img src={searchIcon(engine)} alt="" />{:else}<IconWorld />{/if}</span>
                                <span class="source-name"><strong>{engine.name}</strong>{#if engine.default}<span>Current search engine</span>{/if}</span>
                            </label>
                        {/each}
                    </fieldset>
                    {#if searchLoadError || (!defaultsLoading && !searchEngines.length)}<p class="error" role="alert">Search engines could not be loaded. Keep your current engine or try again.</p>{/if}
                    <div class="discovery-actions"><button id="refresh-search-engines" class="quiet" disabled={busy || defaultsLoading} onclick={loadDefaults}>Refresh search engines</button>{#if searchChanged}<button id="keep-current-search" class="quiet" disabled={busy || defaultsLoading} onclick={keepCurrentSearch}>Keep current search engine</button>{/if}</div>
                    <p class="privacy-hint">You can change this later in Settings → Search engine.</p>
                {:else}
                    <div class="finish-symbol" aria-hidden="true"><NoventraLogo height="42px" /></div>
                    <h1 id="step-title" tabindex="-1" bind:this={title}>You’re ready to browse.</h1>
                    <p class="intro">Finish setup to copy your selected data.</p>
                    {#if cookieResult}<p id="finish-cookies-result" class="import-message" role="status">{cookieResult}</p>{/if}
                    <div class="summary-row" id="data-import-summary"><div><strong>Browser data</strong><p>{stagedKinds.length ? `${sourceLabel}: ${stagedDescription}.` : bookmarkImports || historyImports || passwordImports ? 'Selected browser imports finished.' : 'Import later in Settings.'}</p></div><span class="summary-tag">{importError ? 'Check needed' : stagedKinds.length ? 'Queued' : bookmarkImports || historyImports || passwordImports ? 'Finished' : 'Optional'}</span></div>
                    {#if passwordOpened}<div class="summary-row" id="password-import-summary"><div><strong>Password CSV</strong><p>Check the results in your password manager tab.</p></div><span class="summary-tag">Opened</span></div>{/if}
                    <div class="summary-row"><div><strong>Password manager</strong><p role="status">{managerLoading ? 'Checking extensions…' : managerCheckFailed ? 'Couldn’t check extensions. Go back to retry.' : enabledManagers.length ? `${enabledManagers.join(', ')} installed. Sign in through the extension.` : 'Add an extension later.'}</p></div><span class="summary-tag">{managerLoading ? 'Checking' : managerCheckFailed ? 'Check needed' : enabledManagers.length ? 'Installed' : 'Optional'}</span></div>
                    <div id="browsing-defaults" class="browsing-defaults">
                        <div class="summary-row" id="search-engine-summary"><div><strong>Search engine</strong><p>{searchEngines.find(engine => engine.id === selectedSearchEngineId)?.name || 'Keep current search engine'}</p></div><span class="summary-tag">{searchChanged ? 'Selected' : 'Current'}</span></div>
                        {#if defaultBrowserInfo?.isDefault}<p id="already-default-browser">Noventra is already your default browser.</p>
                        {:else}<label class="default-browser-choice"><input id="setup-make-default" type="checkbox" checked={makeDefault} disabled={busy || defaultsLoading || (!makeDefault && (defaultLoadError || !defaultBrowserInfo?.canBeDefault || defaultBrowserInfo?.isDisabledByPolicy || defaultBrowserInfo?.isUnknownError))} onchange={event => { makeDefault = event.currentTarget.checked; importError = ''; }} />Make Noventra my default browser</label><p>Your operating system may ask you to confirm.</p>{/if}
                        {#if defaultLoadError || defaultBrowserInfo?.isDisabledByPolicy || defaultBrowserInfo?.isUnknownError}<p><a href="chrome://settings/defaultBrowser" target="_blank" rel="noopener noreferrer">Check default browser settings</a></p>{/if}
                        {#if searchChanged || makeDefault || defaultsApplyFailed}<button id="setup-keep-current-defaults" class="quiet" disabled={busy || defaultsLoading} onclick={keepCurrentDefaults}>Keep current browsing defaults</button>{/if}
                    </div>
                    {#if finishing}<p id="setup-import-progress" class="import-message" role="status" aria-live="polite">{importProgress || 'Saving setup…'} Keep this page open.</p>{/if}
                    {#if importError}<p id="setup-import-error" class="error" role="alert">{importError}</p>{/if}
                    {#if interruptedImport && !finishing}<p id="interrupted-import-warning" class="error" role="alert">An import was interrupted. Some data may already have been copied. Check your data before retrying.</p>{/if}
                    {#if fileImportIssue}<p class="error" role="alert">A file import didn’t finish. Check your bookmarks or go back to import the file again.</p>{/if}
                {/if}
                {#if saveError}<p class="error" role="alert">Setup couldn’t be saved. Please try again.</p>{/if}
            </div>
            <footer>
                {#if step > 0}<button id="back" class="quiet" onclick={() => go(step - 1)} disabled={busy}><span aria-hidden="true">←</span> Back</button>{:else}<span class="footer-note">Setup is optional.</span>{/if}
                <button id="continue" class="primary" disabled={busy} onclick={() => step === steps.length - 1 ? finish() : go(step + 1)}>{finishing || saving ? 'Finishing…' : step === 0 ? 'Get started' : step === steps.length - 1 ? importError || interruptedImport ? 'Retry and finish' : stagedKinds.length ? 'Import and finish' : 'Finish setup' : step === 2 && !enabledManagers.length ? 'Continue without installing' : 'Continue'}<span aria-hidden="true">→</span></button>
            </footer>
        </section>
    </div>
</main>

<style>
    :global(body) { margin:0;overflow:auto; }
    :global(button),:global(input),:global(select) { font:inherit; }
    main { --surface:var(--color-primary-background,#fafafb);--ink:contrast-color(var(--surface));--muted:color-mix(in srgb,var(--ink) 62%,var(--surface));--line:color-mix(in srgb,var(--ink) 12%,var(--surface));--soft:color-mix(in srgb,var(--ink) 4%,var(--surface));color:var(--ink);background:var(--surface);min-height:100dvh;padding:24px 32px 32px;box-sizing:border-box; }
    .brand-bar { max-width:1040px;margin:0 auto 26px;display:flex;align-items:center;justify-content:space-between;gap:16px; }
    .brand { display:flex;align-items:center;gap:9px;font-size:20px;font-weight:650;letter-spacing:-.6px; }
    button { cursor:pointer;border:0;color:inherit;background:none;display:flex;align-items:center;justify-content:center;gap:10px;min-height:40px;border-radius:10px;transition:background .15s,transform .15s; }
    button:focus-visible,input:focus-visible,a:focus-visible,summary:focus-visible { outline:2px solid var(--ink);outline-offset:4px; }
    button:disabled { cursor:default;opacity:.45; }
    button:not(:disabled):active { transform:translateY(1px); }
    .quiet { color:var(--muted);padding:7px 11px;font-size:13px; }
    .quiet:hover:not(:disabled) { color:var(--ink);background:var(--soft); }
    .setup-shell { max-width:860px;margin:auto; }
    .steps { list-style:none;padding:0;display:flex;justify-content:space-between;gap:12px;margin:0 4px 20px; }
    .steps li { display:flex;align-items:center;gap:8px;color:var(--muted);font-size:12px;min-width:0; }
    .step-number { display:grid;place-items:center;border:1px solid var(--line);border-radius:50%;width:25px;height:25px;font-size:11px;flex-shrink:0; }
    .steps .active { color:var(--ink);font-weight:650; }.active .step-number { background:var(--ink);color:var(--surface);border-color:var(--ink); }.complete .step-number { background:var(--soft); }
    .panel { border:1px solid var(--line);border-radius:20px;min-width:0;background:var(--surface);overflow:hidden; }
    .panel-content { padding:28px 32px 18px;min-height:340px; }
    .step-count { display:flex;justify-content:space-between;gap:16px;margin-bottom:22px;font-size:10px;letter-spacing:1px;font-weight:600;color:var(--muted); }.step-count span { font-size:9px; }
    h1 { font-size:30px;font-weight:570;line-height:1.18;letter-spacing:-.9px;margin:0 0 12px;outline:none; }
    .intro { color:var(--muted);line-height:1.5;font-size:14px;margin:0 0 22px; }
    .welcome-logo { margin:12px 0 28px; }
    .transfer-link { display:inline-block;color:var(--muted);font-size:13px;line-height:1.5;margin-top:12px;text-underline-offset:3px; }
    footer { display:flex;align-items:center;justify-content:space-between;gap:16px;padding:16px 24px;border-top:1px solid var(--line);background:var(--soft); }.footer-note { font-size:11px;color:var(--muted); }
    .primary { background:var(--ink);color:var(--surface);padding:10px 17px;font-size:13px;font-weight:550; }.primary:hover:not(:disabled) { opacity:.86; }
    .source-heading { display:flex;justify-content:space-between;gap:16px;font-size:12px;margin:10px 0; }.source-heading span { color:var(--muted);font-size:11px; }
    .discovery-actions { display:flex;justify-content:flex-end;margin-top:5px; }
    .data-types { display:flex;align-items:center;gap:12px 24px;flex-wrap:wrap;border:0;padding:0;margin:12px 0 4px;font-size:12px; }.data-types legend { padding:0;margin-bottom:9px;color:var(--muted);font-size:11px; }.data-types label { display:flex;align-items:center;gap:8px;min-height:30px; }.data-types input { margin:0;accent-color:var(--ink); }.data-types span { color:var(--muted);font-size:10px; }
    .profile-list { display:grid;gap:6px;border:0;padding:0;margin:0;max-height:230px;overflow:auto; }.profile-card { display:flex;align-items:center;gap:10px;padding:10px 12px;border:1px solid var(--line);border-radius:10px;cursor:pointer; }.profile-card.selected { border-color:var(--ink);background:var(--soft); }.profile-card.imported { opacity:.6;cursor:default; }.profile-card input { accent-color:var(--ink);margin:0; }
    .source-icon { display:grid;place-items:center;width:32px;height:32px;flex-shrink:0; }.source-icon img { width:28px;height:28px;object-fit:contain; }.source-icon :global(svg) { width:24px;height:24px; }
    .source-name { display:flex;flex-direction:column;gap:3px;flex:1;min-width:0;text-align:left; }.source-name strong { font-size:13px;font-weight:600;overflow-wrap:anywhere; }.source-name>span { font-size:11px;color:var(--muted);overflow-wrap:anywhere; }.source-kind { color:var(--muted);font-size:10px; }
    .empty { display:flex;align-items:center;justify-content:center;flex-direction:column;gap:8px;border:1px dashed var(--line);border-radius:10px;padding:23px 16px;text-align:center;font-size:12px;color:var(--muted); }.empty strong { color:var(--ink);font-size:13px;font-weight:500; }.empty p { margin:0;line-height:1.5; }
    .search-engines { display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:8px;border:0;padding:0;margin:0;max-height:340px;overflow:auto; }
    .search-engine-card { display:flex;align-items:center;gap:10px;padding:12px;border:1px solid var(--line);border-radius:10px;cursor:pointer;min-height:44px; }.search-engine-card.selected { border-color:var(--ink);background:var(--soft); }.search-engine-card input { margin:0;accent-color:var(--ink); }.search-engine-card:has(input:disabled) { opacity:.65;cursor:default; }
    @media(max-width:600px) { .search-engines { grid-template-columns:1fr;max-height:360px; } }
    .browsing-defaults label { display:block;margin:10px 0 6px; }.browsing-defaults .default-browser-choice { display:flex;align-items:center;gap:8px; }.browsing-defaults input { accent-color:var(--ink); }
    .file-fallbacks .file-card { margin-top:10px; }.file-fallbacks p { margin-bottom:10px; }
    .file-card { border:1px solid var(--line);padding:10px 12px;border-radius:10px;width:100%;justify-content:flex-start;gap:10px;text-align:left; }.file-card:hover:not(:disabled) { background:var(--soft); }
    .mac-import-notice { border:1px dashed var(--line);border-radius:10px;padding:10px 12px;margin-top:10px; }.mac-import-notice p { color:var(--muted);font-size:12px;line-height:1.5;margin:0; }.mac-import-notice .quiet { margin-top:5px;min-height:36px;padding-left:0;font-size:12px; }
    .import-message { min-height:18px;margin-top:12px;font-size:12px;color:var(--muted);line-height:1.5; }.success { color:var(--ink); }.error { color:light-dark(#b3261e,#ffb4ab);font-size:12px; }.privacy-hint { font-size:11px;color:var(--muted);line-height:1.5;margin:10px 0; }
    .import-details { margin-top:14px;color:var(--muted);font-size:11px;line-height:1.5; }.import-details summary { cursor:pointer;width:fit-content; }.import-details p { max-width:650px;margin:8px 0 0; }
    .finish-symbol { margin:4px 0 22px; }.summary-row { display:flex;align-items:center;gap:12px;padding:14px 0;border-bottom:1px solid var(--line); }.summary-row>div { flex:1; }.summary-row strong { font-size:13px;font-weight:550; }.summary-row p { font-size:12px;color:var(--muted);margin:4px 0 0;line-height:1.5; }.summary-tag { border:1px solid var(--line);border-radius:6px;padding:4px 7px;font-size:10px;color:var(--muted); }
    .sr-only { position:absolute;width:1px;height:1px;padding:0;overflow:hidden;clip:rect(0,0,0,0);white-space:nowrap;border:0; }
    .spinner { width:20px;height:20px;border:2px solid var(--line);border-top-color:var(--ink);border-radius:50%;animation:spin 1s linear infinite; }@keyframes spin { to { transform:rotate(360deg); } }
    @media(max-width:600px) { main{padding:18px 14px 24px;}.brand-bar{margin-bottom:20px;}.steps{gap:5px;margin-bottom:16px;}.steps li{font-size:10px;gap:5px;flex-direction:column;text-align:center;}.step-number{width:24px;height:24px;font-size:10px;}.steps li>span:last-child{max-width:66px;}.panel-content{padding:24px 20px 16px;min-height:0;}.panel{border-radius:16px;}h1{font-size:27px;}.intro{font-size:13px;margin-bottom:18px;}footer{padding:14px 18px;gap:10px;}.primary{padding:10px 14px;font-size:12px;}.source-kind{max-width:84px;text-align:right;}.welcome-logo{margin:8px 0 24px;}.summary-tag{font-size:9px;} }
    @media(max-width:380px) { .source-kind{max-width:65px;font-size:9px;}.source-icon{width:26px;height:28px;}.source-icon img{width:25px;height:25px;}.profile-card{gap:7px;padding:10px;}.step-count span{display:none;}.source-name strong{font-size:12px;}.source-name>span{font-size:10px;} }
    @media(prefers-reduced-motion:reduce) { button{transition:none;}.spinner{animation:none;} }
</style>
