/// <reference types="svelte" />
/// <reference types="vite/client" />
/// <reference path="./lib/cr/cr.d.ts" />


declare module 'chrome://resources/cr_components/color_change_listener/colors_css_updater.js' {
    export class ColorChangeUpdater {
        static forDocument(): ColorChangeUpdater;
        start(): void;
    }
}
