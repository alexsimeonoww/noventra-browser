import { mount } from 'svelte'
import './lib/chrome-send-polyfill';

import "@fontsource-variable/instrument-sans/index.css";
import "@imput/helium-prism/styles.css";
import './app.css'
import { initStrings, s } from './lib/strings';

// Chromium supplies this module; keep it external to the Vite bundle.
const { ColorChangeUpdater } = await import(/* @vite-ignore */
    'chrome://resources/cr_components/color_change_listener/colors_css_updater.js');
ColorChangeUpdater.forDocument().start();

await initStrings();
document.title = s.page.title;
const { default: App } = await import('./App.svelte');

const app = mount(App, {
    target: document.getElementById('app')!,
})

export default app
