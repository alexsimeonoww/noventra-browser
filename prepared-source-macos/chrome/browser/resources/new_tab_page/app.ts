// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import '/strings.m.js';
import './ntp_searchbox.js';
import 'chrome://new-tab-page/shared/customize_buttons/customize_buttons.js';
import 'chrome://resources/cr_components/composebox/threads_rail.js';
import 'chrome://resources/cr_components/composebox/composebox_voice_search.js';
import 'chrome://resources/cr_components/search/animated_glow.js';

import {SearchboxBrowserProxy} from '//resources/cr_components/searchbox/searchbox_browser_proxy.js';
import type {CustomizeButtonsElement} from 'chrome://new-tab-page/shared/customize_buttons/customize_buttons.js';
import {ColorChangeUpdater} from 'chrome://resources/cr_components/color_change_listener/colors_css_updater.js';
import {GlifAnimationState} from 'chrome://resources/cr_components/composebox/common.js';
import type {ComposeboxState} from 'chrome://resources/cr_components/composebox/common.js';
import {VoiceSearchAction as ComposeVoiceSearchAction} from 'chrome://resources/cr_components/composebox/composebox.js';
import type {ComposeboxVoiceSearchElement, VoicePermissionPromptState} from 'chrome://resources/cr_components/composebox/composebox_voice_search.js';
import {HelpBubbleMixinLit} from 'chrome://resources/cr_components/help_bubble/help_bubble_mixin_lit.js';
import type {SearchAnimatedGlowElement} from 'chrome://resources/cr_components/search/animated_glow.js';
import type {CrToastElement} from 'chrome://resources/cr_elements/cr_toast/cr_toast.js';
import {assert, assertNotReached} from 'chrome://resources/js/assert.js';
import {hexColorToSkColor, skColorToRgba} from 'chrome://resources/js/color_utils.js';
import {EventTracker} from 'chrome://resources/js/event_tracker.js';
import {FocusOutlineManager} from 'chrome://resources/js/focus_outline_manager.js';
import {loadTimeData} from 'chrome://resources/js/load_time_data.js';
import {getTrustedScriptURL} from 'chrome://resources/js/static_types.js';
import {CrLitElement} from 'chrome://resources/lit/v3_0/lit.rollup.js';
import type {PropertyValues} from 'chrome://resources/lit/v3_0/lit.rollup.js';
import type {FuseboxAction} from 'chrome://resources/mojo/components/omnibox/browser/fusebox_action.mojom-webui.js';
import {SearchboxOverride} from 'chrome://resources/mojo/components/omnibox/browser/fusebox_action.mojom-webui.js';
import type {PageCallbackRouter as SearchboxPageCallbackRouter} from 'chrome://resources/mojo/components/omnibox/browser/searchbox.mojom-webui.js';
import type {SkColor} from 'chrome://resources/mojo/skia/public/mojom/skcolor.mojom-webui.js';

import type {ActionChipClickDetail} from './action_chips/action_chips.js';
import {ActionChipsRetrievalState} from './action_chips/action_chips.js';
import {getCss} from './app.css.js';
import {getHtml} from './app.html.js';
import {BackgroundManager} from './background_manager.js';
import type {CustomizeButtonsDocumentCallbackRouter, CustomizeButtonsHandlerRemote} from './customize_buttons.mojom-webui.js';
import {SidePanelOpenTrigger} from './customize_buttons.mojom-webui.js';
import {CustomizeButtonsProxy} from './customize_buttons_proxy.js';
import {CustomizeChromeSection} from './customize_chrome.mojom-webui.js';
import {CustomizeDialogPage} from './customize_dialog_types.js';
import {recordBoolean, recordDuration, recordEnumeration, recordLinearValue, recordLoadDuration, recordSparseValueWithPersistentHash} from './metrics_utils.js';
import {ParentTrustedDocumentProxy} from './modules/microsoft_auth_frame_connector.js';
import type {NoventraForegroundRect, PageCallbackRouter, PageHandlerRemote, Theme} from './new_tab_page.mojom-webui.js';
import {NtpBackgroundImageSource} from './new_tab_page.mojom-webui.js';
import {NewTabPageProxy} from './new_tab_page_proxy.js';
import type {NtpComposeboxElement} from './ntp_composebox.js';
import {ShowNtpPromosResult} from './ntp_promo.mojom-webui.js';
import {$$} from './utils.js';
import {Action as VoiceAction, recordVoiceAction} from './voice_search_overlay.js';
import {WindowProxy} from './window_proxy.js';

enum ModuleLoadStatus {
  MODULE_LOAD_IN_PROGRESS = 0,
  MODULE_LOAD_NOT_ATTEMPTED = 1,
  MODULE_LOAD_COMPLETE = 2,
}

/**
 * Elements on the NTP. This enum must match the numbering for NTPElement in
 * enums.xml. These values are persisted to logs. Entries should not be
 * renumbered, removed or reused.
 */
export enum NtpElement {
  OTHER = 0,
  BACKGROUND = 1,
  ONE_GOOGLE_BAR = 2,
  LOGO = 3,
  REALBOX = 4,
  MOST_VISITED = 5,
  MIDDLE_SLOT_PROMO = 6,  // Obsolete
  MODULE = 7,
  CUSTOMIZE = 8,  // Obsolete
  CUSTOMIZE_BUTTON = 9,
  CUSTOMIZE_DIALOG = 10,  // Obsolete
  WALLPAPER_SEARCH_BUTTON = 11,
  ACTION_CHIPS = 12,
  THREADS_RAIL = 13,
  MAX_VALUE = THREADS_RAIL,
}

/**
 * Customize Chrome entry points. This enum must match the numbering for
 * NtpCustomizeChromeEntryPoint in enums.xml. These values are persisted to
 * logs. Entries should not be renumbered, removed or reused.
 */
export enum NtpCustomizeChromeEntryPoint {
  CUSTOMIZE_BUTTON = 0,
  MODULE = 1,
  URL = 2,
  WALLPAPER_SEARCH_BUTTON = 3,
  MAX_VALUE = WALLPAPER_SEARCH_BUTTON,
}

/**
 * Defines the conditions that hide the wallpaper search button on the New Tab
 * Page.
 */
enum NtpWallpaperSearchButtonHideCondition {
  NONE = 0,
  BACKGROUND_IMAGE_SET = 1,
  THEME_SET = 2,
  MAX_VALUE = THEME_SET,
}

const CUSTOMIZE_URL_PARAM: string = 'customize';
const MSAL_IFRAME_ORIGIN = 'chrome-untrusted://ntp-microsoft-auth';
const VOICE_QUERY_LENGTH_LIMIT = 120;
const VOICE_AUTO_SUBMIT_IDLE_TIMEOUT_MS = 3000;
const VOICE_MANUAL_SUBMIT_IDLE_TIMEOUT_MS = 10000;
const COMPOSEBOX_INERT_ALLOWLIST = [
  '#logo',
  '#searchboxContainer',
];

export const CUSTOMIZE_CHROME_BUTTON_ELEMENT_ID =
    'CustomizeButtonsHandler::kCustomizeChromeButtonElementId';

// 900px ~= 561px (max value for --ntp-search-box-width) * 1.5 + some margin.
const realboxCanShowSecondarySideMediaQueryList =
    window.matchMedia('(min-width: 900px)');

function recordClick(element: NtpElement) {
  recordEnumeration('NewTabPage.Click', element, NtpElement.MAX_VALUE + 1);
}

function recordCustomizeChromeOpen(element: NtpCustomizeChromeEntryPoint) {
  recordEnumeration(
      'NewTabPage.CustomizeChromeOpened', element,
      NtpCustomizeChromeEntryPoint.MAX_VALUE + 1);
}

// Adds a <script> tag that holds the lazy loaded code.
function ensureLazyLoaded() {
  const script = document.createElement('script');
  script.type = 'module';
  script.src = getTrustedScriptURL`./lazy_load.js`;
  document.body.appendChild(script);
}

function recordShowBrowserPromosResult(result: ShowNtpPromosResult) {
  recordEnumeration(
      'UserEducation.NtpPromos.ShowResult', result,
      ShowNtpPromosResult.MAX_VALUE + 1);
}

const PERMISSION_PROMPT_CSS_CLASS = 'permission-prompt-showing';

const AppElementBase = HelpBubbleMixinLit(CrLitElement);

export interface AppElement {
  $: {
    customizeButtons: CustomizeButtonsElement,
  };
}

export class AppElement extends AppElementBase {
  static get is() {
    return 'ntp-app';
  }

  static override get styles() {
    return getCss();
  }

  override render() {
    return getHtml.bind(this)();
  }

  static override get properties() {
    return {
      // =======================================================================
      // Public properties
      // =======================================================================

      /**
       * Whether the secondary side can be shown based on the feature state and
       * the width available to the dropdown for the ntp searchbox.
       */
      realboxCanShowSecondarySide: {
        type: Boolean,
        reflect: true,
      },

      /**
       * Whether the searchbox secondary side was at any point available to
       * be shown.
       */
      realboxHadSecondarySide: {
        type: Boolean,
        reflect: true,
        notify: true,
      },

      composeButtonEnabled: {type: Boolean},
      composeboxEnabled: {type: Boolean},

      hasVoiceSearchError: {type: Boolean},

      // =======================================================================
      // Protected properties
      // =======================================================================

      composeboxState_: {type: Object},
      theme_: {type: Object},
      showCustomize_: {type: Boolean},
      showCustomizeChromeText_: {type: Boolean},
      showWallpaperSearch_: {type: Boolean},
      showVoiceSearchOverlay_: {type: Boolean},
      showVoiceSearchScrim_: {
        type: Boolean,
        reflect: true,
      },
      voiceSearchCoherenceAnySearchboxExperimentEnabled_: {type: Boolean},
      voiceSearchCoherenceSearchboxWithLiveTranscriptionEnabled_:
          {type: Boolean},
      voiceSearchCoherenceRealboxAutoEndpointEnabled_: {type: Boolean},
      voiceSearchCoherenceRealboxHelperTextEnabled_: {type: Boolean},

      voiceSearchTranscript_: {type: String},
      voiceSearchReceivedSpeech_: {type: Boolean},
      voiceSearchListening_: {type: Boolean},
      voiceQueryLengthLimit_: {type: Number},
      voiceAutoSubmitIdleTimeoutMs_: {type: Number},
      voiceManualSubmitIdleTimeoutMs_: {type: Number},
      searchboxCallbackRouter_: {type: Object},

      showBackgroundImage_: {
        reflect: true,
        type: Boolean,
      },

      backgroundImageAttribution1_: {type: String},
      backgroundImageAttribution2_: {type: String},
      backgroundImageAttributionUrl_: {type: String},

      // Used in cr-searchbox component via host-context.
      colorSourceIsBaseline: {type: Boolean},
      logoColor_: {type: Object},
      singleColoredLogo_: {type: Boolean},

      realboxShown_: {type: Boolean},

      /**
       * Whether the composebox has been opened at least once.
       */
      wasComposeboxOpened_: {type: Boolean},

      showLensUploadDialog_: {type: Boolean},

      showComposebox_: {
        type: Boolean,
        reflect: true,
      },

      logoEnabled_: {type: Boolean},
      oneGoogleBarEnabled_: {type: Boolean},
      shortcutsEnabled_: {type: Boolean},
      modulesEnabled_: {type: Boolean},

      browserPromoType_: {type: String},
      browserPromoLimit_: {type: Number},
      browserPromoCompletedLimit_: {type: Number},
      showBrowserPromo_: {type: Boolean},
      caretAnimationsEnabled_: {type: Boolean},
      usePecApi_: {type: Boolean},
      smartTabSharingVisible_: {type: Boolean},
      contextManagementInComposeboxEnabled_: {type: Boolean},

      modulesShownToUser: {
        type: Boolean,
        reflect: true,
      },

      microsoftModuleEnabled_: {type: Boolean},
      microsoftAuthIframePath_: {type: String},

      multiLineEnabled_: {type: Boolean},

      /**
       * Modules are hidden until loaded.
       */
      modulesLoaded_: {type: Boolean},

      /**
       * If true, renders additional elements that were not deemed crucial to
       * to show up immediately on load.
       */
      lazyRender_: {type: Boolean},

      scrolledToTop_: {type: Boolean},

      wallpaperSearchButtonAnimationEnabled_: {type: Boolean},
      wallpaperSearchButtonEnabled_: {type: Boolean},
      showWallpaperSearchButton_: {type: Boolean},
      isActionChipsVisible_: {type: Boolean},
      isFooterVisible_: {type: Boolean},

      ntpRealboxNextEnabled_: {
        type: Boolean,
        reflect: true,
      },

      searchboxCyclingPlaceholders_: {
        type: Boolean,
      },

      ntpNextFeaturesEnabled_: {type: Boolean},
      maxTilesInCollapsedState_: {type: Number},
      maxShortcutsInExpandedState_: {type: Number},
      maxMostVisitedTilesInExpandedState_: {type: Number},
      maxEnterpriseShortcuts_: {type: Number},
      containerFocused_: {type: Boolean},

      /**
       * Whether the scrim is shown in Realbox Next.
       */
      showScrim_: {
        type: Boolean,
        reflect: true,
      },

      contextMenuGlifAnimationState_: {type: String},
      undoToastCallback_: {type: Object},
      undoToastMessage_: {type: String},

      /**
       * Whether to show the AIM threads rail when composebox is open.
       */
      enableThreadsRail_: {type: Boolean},

      // =======================================================================
      // Private properties
      // =======================================================================

      selectedCustomizeDialogPage_: {type: String},
      modulesLoadedStatus_: {
        type: Number,
        reflect: true,
      },

      energyEffectEnabled_: {type: Boolean, reflect: true},
      energyEffectAnimationEnabled_: {type: Boolean, reflect: true},
      energyEffectVariant_: {type: String, reflect: true},
      showCustomizeButton_: {type: Boolean},
    };
  }

  accessor hasVoiceSearchError = false;
  accessor realboxCanShowSecondarySide: boolean = false;
  accessor realboxHadSecondarySide: boolean = false;
  accessor composeButtonEnabled: boolean =
      loadTimeData.getBoolean('searchboxShowComposeEntrypoint');
  accessor composeboxEnabled: boolean =
      loadTimeData.getBoolean('searchboxShowComposebox');

  protected accessor composeboxState_: ComposeboxState|null = null;
  protected accessor theme_: Theme|null = null;
  protected accessor showCustomize_: boolean = false;
  protected accessor showCustomizeChromeText_: boolean = false;
  protected accessor showWallpaperSearch_: boolean = false;
  protected accessor showVoiceSearchOverlay_: boolean = false;
  protected accessor showVoiceSearchScrim_: boolean = false;
  protected accessor voiceSearchCoherenceAnySearchboxExperimentEnabled_:
      boolean = loadTimeData.getBoolean(
                    'voiceSearchCoherenceAnySearchboxExperimentEnabled') ||
      loadTimeData.getBoolean(
          'voiceSearchCoherenceSearchboxWithLiveTranscriptionEnabled');
  protected accessor voiceSearchCoherenceSearchboxWithLiveTranscriptionEnabled_:
      boolean = loadTimeData.getBoolean(
          'voiceSearchCoherenceSearchboxWithLiveTranscriptionEnabled');
  protected accessor voiceSearchCoherenceRealboxAutoEndpointEnabled_: boolean =
      loadTimeData.getBoolean('voiceSearchCoherenceRealboxAutoEndpointEnabled');
  protected accessor voiceSearchCoherenceRealboxHelperTextEnabled_: boolean =
      loadTimeData.getBoolean('voiceSearchCoherenceRealboxHelperTextEnabled');

  protected accessor voiceSearchTranscript_: string = '';
  protected accessor voiceSearchReceivedSpeech_: boolean = false;
  protected accessor voiceSearchListening_: boolean = false;
  protected accessor voiceQueryLengthLimit_: number = VOICE_QUERY_LENGTH_LIMIT;
  protected accessor voiceAutoSubmitIdleTimeoutMs_: number =
      VOICE_AUTO_SUBMIT_IDLE_TIMEOUT_MS;
  protected accessor voiceManualSubmitIdleTimeoutMs_: number =
      VOICE_MANUAL_SUBMIT_IDLE_TIMEOUT_MS;
  protected accessor showBackgroundImage_: boolean = false;
  protected accessor backgroundImageAttribution1_: string = '';
  protected accessor backgroundImageAttribution2_: string = '';
  protected accessor backgroundImageAttributionUrl_: string = '';
  protected accessor colorSourceIsBaseline: boolean = false;
  protected accessor logoColor_: SkColor|null = null;
  protected accessor singleColoredLogo_: boolean = false;
  protected accessor realboxShown_: boolean = false;
  protected accessor wasComposeboxOpened_: boolean = false;
  protected accessor showLensUploadDialog_: boolean = false;
  protected accessor showComposebox_: boolean = false;
  protected accessor caretAnimationsEnabled_: boolean =
      loadTimeData.getBoolean('caretAnimationEnabled');
  protected accessor usePecApi_: boolean =
      loadTimeData.getBoolean('contextualMenuUsePecApi');
  protected accessor smartTabSharingVisible_: boolean =
      loadTimeData.getBoolean('composeboxSmartTabSharingVisible');
  protected accessor contextManagementInComposeboxEnabled_: boolean =
      loadTimeData.getBoolean('contextManagementInComposeboxEnabled');
  protected accessor logoEnabled_: boolean =
      loadTimeData.getBoolean('logoEnabled');
  protected accessor oneGoogleBarEnabled_: boolean =
      loadTimeData.getBoolean('oneGoogleBarEnabled');
  protected accessor shortcutsEnabled_: boolean =
      loadTimeData.getBoolean('shortcutsEnabled');
  protected accessor modulesEnabled_: boolean =
      loadTimeData.getBoolean('modulesEnabled');
  protected accessor browserPromoType_: string =
      loadTimeData.getString('browserPromoType');
  protected accessor browserPromoLimit_: number =
      loadTimeData.getInteger('browserPromoLimit');
  protected accessor browserPromoCompletedLimit_: number =
      loadTimeData.getInteger('browserPromoCompletedLimit');
  protected accessor showBrowserPromo_: boolean = false;
  protected accessor modulesShownToUser: boolean = false;
  protected accessor microsoftModuleEnabled_: boolean =
      loadTimeData.getBoolean('microsoftModuleEnabled');
  protected accessor microsoftAuthIframePath_: string = MSAL_IFRAME_ORIGIN;
  protected accessor multiLineEnabled_: boolean =
      loadTimeData.getBoolean('multiLineEnabled');
  protected accessor modulesLoaded_: boolean = false;
  protected accessor lazyRender_: boolean = false;
  protected accessor scrolledToTop_: boolean =
      document.documentElement.scrollTop <= 0;
  protected accessor wallpaperSearchButtonAnimationEnabled_: boolean =
      loadTimeData.getBoolean('wallpaperSearchButtonAnimationEnabled');
  protected accessor wallpaperSearchButtonEnabled_: boolean =
      loadTimeData.getBoolean('wallpaperSearchButtonEnabled');
  protected accessor showWallpaperSearchButton_: boolean = false;
  protected accessor isActionChipsVisible_: boolean =
      loadTimeData.getBoolean('actionChipsEnabled');
  protected accessor isFooterVisible_: boolean = false;
  protected accessor ntpRealboxNextEnabled_: boolean =
      loadTimeData.getBoolean('ntpRealboxNextEnabled');
  protected accessor searchboxCyclingPlaceholders_: boolean =
      loadTimeData.getBoolean('searchboxCyclingPlaceholders');
  protected accessor ntpNextFeaturesEnabled_: boolean =
      loadTimeData.getBoolean('ntpNextFeaturesEnabled');
  protected accessor maxTilesInCollapsedState_: number =
      loadTimeData.getInteger('maxTilesInCollapsedState');
  protected accessor maxShortcutsInExpandedState_: number =
      loadTimeData.getInteger('maxShortcutsInExpandedState');
  protected accessor maxMostVisitedTilesInExpandedState_: number =
      loadTimeData.getInteger('maxMostVisitedTilesInExpandedState');
  protected accessor maxEnterpriseShortcuts_: number =
      loadTimeData.getInteger('maxEnterpriseShortcuts');
  protected accessor containerFocused_: boolean = false;
  protected accessor showScrim_: boolean = false;
  protected realboxContextMenuAnimationAllowed_: boolean = false;
  protected accessor contextMenuGlifAnimationState_: GlifAnimationState =
      GlifAnimationState.INELIGIBLE;
  protected accessor undoToastCallback_: (() => void)|null = null;
  protected accessor undoToastMessage_: string|null = null;
  protected accessor enableThreadsRail_: boolean =
      loadTimeData.getBoolean('enableThreadsRail');
  protected accessor energyEffectEnabled_: boolean =
      loadTimeData.getBoolean('energyEffectEnabled');
  protected accessor energyEffectAnimationEnabled_: boolean =
      loadTimeData.getBoolean('energyEffectAnimationEnabled');
  protected accessor energyEffectVariant_: string =
      loadTimeData.getString('energyEffectVariant');
  protected accessor showCustomizeButton_: boolean =
      loadTimeData.getBoolean('showCustomizeButton');
  protected contextMenuAnimationLimitingEnabled_: boolean =
      loadTimeData.getBoolean('contextMenuAnimationLimitingEnabled');
  protected accessor searchboxCallbackRouter_: SearchboxPageCallbackRouter;

  private accessor selectedCustomizeDialogPage_: string|null = null;
  private accessor modulesLoadedStatus_: ModuleLoadStatus =
      ModuleLoadStatus.MODULE_LOAD_IN_PROGRESS;
  private callbackRouter_: PageCallbackRouter;
  private pageHandler_: PageHandlerRemote;
  private customizeButtonsCallbackRouter_:
      CustomizeButtonsDocumentCallbackRouter;
  private customizeButtonsHandler_: CustomizeButtonsHandlerRemote;
  private backgroundManager_: BackgroundManager;
  private connectMicrosoftAuthToParentDocumentListenerId_: number|null = null;
  private setThemeListenerId_: number|null = null;
  private setCustomizeChromeSidePanelVisibilityListener_: number|null = null;
  private setWallpaperSearchButtonVisibilityListener_: number|null = null;
  private setActionChipsVisibilityListenerId_: number|null = null;
  private footerVisibilityUpdatedListener_: number|null = null;
  private eventTracker_: EventTracker = new EventTracker();
  private shouldPrintPerformance_: boolean = false;
  private backgroundImageLoadStartEpoch_: number = 0;
  private backgroundImageLoadStart_: number = 0;
  private showWebstoreToastListenerId_: number|null = null;
  private pendingUndoToasts_: Array<{message: string, undo: () => void}> = [];
  private noventraForegroundFrame_: number|null = null;
  private noventraForegroundReport_: string = '';
  private noventraForegroundRetries_: number = 0;
  private noventraForegroundRoots_: Set<ShadowRoot> = new Set();
  private noventraForegroundElements_: Set<Element> = new Set();
  private noventraForegroundMutations_: MutationObserver =
      new MutationObserver(() => this.scheduleNoventraForegroundReport_());
  private noventraForegroundResize_: ResizeObserver =
      new ResizeObserver(() => this.scheduleNoventraForegroundReport_());
  private noventraForegroundScroll_ = () =>
      this.scheduleNoventraForegroundReport_();

  constructor() {
    performance.mark('app-creation-start');
    super();
    this.callbackRouter_ = NewTabPageProxy.getInstance().callbackRouter;
    this.searchboxCallbackRouter_ =
        SearchboxBrowserProxy.getInstance().callbackRouter;
    this.pageHandler_ = NewTabPageProxy.getInstance().handler;
    this.customizeButtonsCallbackRouter_ =
        CustomizeButtonsProxy.getInstance().callbackRouter;
    this.customizeButtonsHandler_ = CustomizeButtonsProxy.getInstance().handler;
    this.backgroundManager_ = BackgroundManager.getInstance();
    this.shouldPrintPerformance_ =
        new URLSearchParams(location.search).has('print_perf');

    this.showCustomize_ =
        WindowProxy.getInstance().url.searchParams.has(CUSTOMIZE_URL_PARAM);

    this.selectedCustomizeDialogPage_ =
        WindowProxy.getInstance().url.searchParams.get(CUSTOMIZE_URL_PARAM);

    this.realboxCanShowSecondarySide =
        realboxCanShowSecondarySideMediaQueryList.matches;

    /**
     * Initialized with the start of the performance timeline in case the
     * background image load is not triggered by JS.
     */
    this.backgroundImageLoadStartEpoch_ = performance.timeOrigin;

    recordLinearValue(
        'NewTabPage.Height',
        /*min=*/ 1,
        /*max=*/ 1000,
        /*buckets=*/ 200,
        /*value=*/ Math.floor(window.innerHeight));
    recordLinearValue(
        'NewTabPage.Width',
        /*min=*/ 1,
        /*max=*/ 1920,
        /*buckets=*/ 384,
        /*value=*/ Math.floor(window.innerWidth));

    ColorChangeUpdater.forDocument().start();

    window.navigator.virtualKeyboard.overlaysContent = true;
  }

  override connectedCallback() {
    super.connectedCallback();
    realboxCanShowSecondarySideMediaQueryList.addEventListener(
        'change', this.onRealboxCanShowSecondarySideChanged_);

    // Listen for chrome-untrusted://ntp-microsoft-auth iframe trying to
    // connect to the NTP.
    this.connectMicrosoftAuthToParentDocumentListenerId_ =
        this.callbackRouter_.connectToParentDocument.addListener(
            childDocumentRemote => {
              ParentTrustedDocumentProxy.setInstance(childDocumentRemote);
            });

    this.setThemeListenerId_ =
        this.callbackRouter_.setTheme.addListener(theme => {
          if (!this.theme_) {
            this.onThemeLoaded_(theme);
          }
          performance.measure('theme-set');
          this.theme_ = theme;
        });
    this.setCustomizeChromeSidePanelVisibilityListener_ =
        this.customizeButtonsCallbackRouter_
            .setCustomizeChromeSidePanelVisibility.addListener(visible => {
              this.showCustomize_ = visible;
              if (!visible) {
                this.showWallpaperSearch_ = false;
              }
            });
    this.showWebstoreToastListenerId_ =
        this.callbackRouter_.showWebstoreToast.addListener(() => {
          if (this.showCustomize_) {
            const toast = $$<CrToastElement>(this, '#webstoreToast');
            if (toast) {
              toast.hidden = false;
              toast.show();
            }
          }
        });
    this.setWallpaperSearchButtonVisibilityListener_ =
        this.callbackRouter_.setWallpaperSearchButtonVisibility.addListener(
            visible => {
              // Hides the wallpaper search button if the browser indicates that
              // it should be hidden.
              // Note: We don't resurface the button later even if the browser
              // says we should, to avoid issues if Customize Chrome doesn't
              // have the wallpaper search element yet.
              if (!visible) {
                this.wallpaperSearchButtonEnabled_ = visible;
              }
            });

    this.setActionChipsVisibilityListenerId_ =
        this.callbackRouter_.setActionChipsVisibility.addListener(
            isVisible => this.isActionChipsVisible_ = isVisible);

    this.footerVisibilityUpdatedListener_ =
        this.callbackRouter_.footerVisibilityUpdated.addListener(visible => {
          this.isFooterVisible_ = visible;
        });
    this.pageHandler_.updateFooterVisibility();

    // Open Customize Chrome if there are Customize Chrome URL params.
    if (this.showCustomize_) {
      this.setCustomizeChromeSidePanelVisible_(this.showCustomize_);
      recordCustomizeChromeOpen(NtpCustomizeChromeEntryPoint.URL);
    }
    this.eventTracker_.add(window, 'keydown', this.onWindowKeydown_.bind(this));
    this.eventTracker_.add(
        window, 'click', this.onWindowClick_.bind(this), /*capture=*/ true);
    this.eventTracker_.add(document, 'scroll', () => {
      this.scrolledToTop_ = document.documentElement.scrollTop <= 0;
      this.scheduleNoventraForegroundReport_();
    });
    const viewportChanged = () => {
      this.sendNoventraForegroundReport_([], false);
      this.scheduleNoventraForegroundReport_();
    };
    this.eventTracker_.add(window, 'resize', viewportChanged);
    if (window.visualViewport) {
      this.eventTracker_.add(window.visualViewport, 'resize', viewportChanged);
      this.eventTracker_.add(window.visualViewport, 'scroll', viewportChanged);
    }
    this.eventTracker_.add(document, 'visibilitychange', () => {
      this.scheduleNoventraForegroundReport_();
    });
    if (loadTimeData.getString('backgroundImageUrl')) {
      this.backgroundManager_.getBackgroundImageLoadTime().then(
          time => {
            const duration = time - this.backgroundImageLoadStartEpoch_;
            recordDuration(
                'NewTabPage.Images.ShownTime.BackgroundImage', duration);
            if (this.shouldPrintPerformance_) {
              this.printPerformanceDatum_(
                  'background-image-load', this.backgroundImageLoadStart_,
                  duration);
              this.printPerformanceDatum_(
                  'background-image-loaded',
                  this.backgroundImageLoadStart_ + duration);
            }
          },
          () => {
              // Ignore. Failed to capture background image load time.
          });
    }
    FocusOutlineManager.forDocument(document);
    if (this.composeButtonEnabled) {
      recordBoolean('NewTabPage.ComposeEntrypoint.Shown', true);
      this.pageHandler_.incrementComposeButtonShownCount();
    }

    this.initializeContextMenuAnimationState_();
  }

  override disconnectedCallback() {
    super.disconnectedCallback();
    if (this.noventraForegroundFrame_ !== null) {
      cancelAnimationFrame(this.noventraForegroundFrame_);
      this.noventraForegroundFrame_ = null;
    }
    this.noventraForegroundMutations_.disconnect();
    this.noventraForegroundResize_.disconnect();
    for (const root of this.noventraForegroundRoots_) {
      root.removeEventListener('scroll', this.noventraForegroundScroll_, true);
    }
    this.noventraForegroundRoots_.clear();
    this.noventraForegroundElements_.clear();
    this.sendNoventraForegroundReport_([], false);
    realboxCanShowSecondarySideMediaQueryList.removeEventListener(
        'change', this.onRealboxCanShowSecondarySideChanged_);

    this.callbackRouter_.removeListener(
        this.connectMicrosoftAuthToParentDocumentListenerId_!);
    this.callbackRouter_.removeListener(this.setThemeListenerId_!);
    this.callbackRouter_.removeListener(this.showWebstoreToastListenerId_!);
    this.callbackRouter_.removeListener(
        this.setWallpaperSearchButtonVisibilityListener_!);
    this.customizeButtonsCallbackRouter_.removeListener(
        this.setCustomizeChromeSidePanelVisibilityListener_!);
    this.callbackRouter_.removeListener(
        this.setActionChipsVisibilityListenerId_!);
    this.callbackRouter_.removeListener(this.footerVisibilityUpdatedListener_!);
    this.eventTracker_.removeAll();
  }

  override willUpdate(changedProperties: PropertyValues<this>) {
    super.willUpdate(changedProperties);

    const changedPrivateProperties =
        changedProperties as Map<PropertyKey, unknown>;

    if (changedPrivateProperties.has('theme_')) {
      this.showBackgroundImage_ = this.computeShowBackgroundImage_();
      this.backgroundImageAttribution1_ =
          this.computeBackgroundImageAttribution1_();
      this.backgroundImageAttribution2_ =
          this.computeBackgroundImageAttribution2_();
      this.backgroundImageAttributionUrl_ =
          this.computeBackgroundImageAttributionUrl_();
      this.colorSourceIsBaseline = this.computeColorSourceIsBaseline();
      this.logoColor_ = this.computeLogoColor_();
      this.singleColoredLogo_ = this.computeSingleColoredLogo_();
    }

    // theme_, showLensUploadDialog_
    this.realboxShown_ = this.computeRealboxShown_();

    // modulesLoadedStatus_
    this.modulesLoaded_ = this.computeModulesLoaded_();

    // wallpaperSearchButtonEnabled_, showBackgroundImage_, backgroundColor_
    this.showWallpaperSearchButton_ = this.computeShowWallpaperSearchButton_();

    // showWallpaperSearchButton_, showBackgroundImage_
    this.showCustomizeChromeText_ = this.computeShowCustomizeChromeText_();

    // modulesEnabled_, modulesShownToUser, modulesLoadedStatus_
    this.showBrowserPromo_ = this.computeShowBrowserPromo_();

    if ((changedPrivateProperties.has('modulesLoadedStatus_') &&
         this.modulesLoadedStatus_ !==
             ModuleLoadStatus.MODULE_LOAD_IN_PROGRESS)) {
      this.recordBrowserPromoMetrics_();
    }

    if (changedPrivateProperties.has('showVoiceSearchOverlay_')) {
      this.showVoiceSearchScrim_ =
          this.voiceSearchCoherenceAnySearchboxExperimentEnabled_ &&
          this.showVoiceSearchOverlay_;
    }

    if (this.ntpRealboxNextEnabled_ && [
          'showComposebox_',
          'showLensUploadDialog_',
          'containerFocused_',
          'showVoiceSearchScrim_',
          'showVoiceSearchOverlay_',
        ].some((prop) => changedPrivateProperties.has(prop))) {
      /**
       * The current requirement is that the scrim should be shown when the
       * focus is placed on one of the input boxes and should be removed when
       * the focus moves outside.
       *
       * The additional OR operation with showComposebox_ is because the logic
       * does not close Composebox when a click outside is made while Composebox
       * is opened. What seems to be happening when showComposebox_ is used/not
       * used are as follows:
       * - Without it:
       *   1. A click outside is made.
       *   2. The focusout event first occurs.
       *   3. containerFocused_ is set to false.
       *   4. The scrim is removed.
       *   5. The click event fires.
       * - With it:
       *   1-3. same as above
       *   4. The scrim is kept since showComposebox_ is still true.
       *   5. The onclick handler of the scrim runs and sets showComposebox_ to
       *      false, and everything works as desired.
       */
      this.showScrim_ = this.showComposebox_ || this.showLensUploadDialog_ ||
          this.containerFocused_ || this.showVoiceSearchScrim_;
    }
  }

  override firstUpdated() {
    this.pageHandler_.onAppRendered(WindowProxy.getInstance().now());
    // Let the browser breathe and then render remaining elements.
    WindowProxy.getInstance().waitForLazyRender().then(() => {
      ensureLazyLoaded();
      this.lazyRender_ = true;
    });
    this.printPerformance_();
    performance.measure('app-creation', 'app-creation-start');

    if (!this.modulesEnabled_) {
      this.recordBrowserPromoMetrics_();
    }

  }

  override updated(changedProperties: PropertyValues<this>) {
    super.updated(changedProperties);
    this.scheduleNoventraForegroundReport_();

    const changedPrivateProperties =
        changedProperties as Map<PropertyKey, unknown>;

    if (changedPrivateProperties.has('lazyRender_') && this.lazyRender_) {
      this.onLazyRendered_();
    }

    if (changedPrivateProperties.has('theme_')) {
      this.onThemeChange_();
    }

    if (changedPrivateProperties.has('isFooterVisible_') && this.lazyRender_) {
      this.maybeRegisterCustomizeButtonHelpBubble_();
    }

    if (changedPrivateProperties.has('logoColor_')) {
      this.style.setProperty(
          '--ntp-logo-color', this.rgbaOrInherit_(this.logoColor_));
    }

    if (changedPrivateProperties.has('showBackgroundImage_')) {
      this.onShowBackgroundImageChange_();
    }

    if (changedPrivateProperties.has('modulesLoaded_')) {
      this.onModulesLoadedChange_();
    }

    if (changedPrivateProperties.has('showComposebox_')) {
      this.onShowComposeboxChange_();
    }


    if (changedPrivateProperties.has('showComposebox_') &&
        this.showComposebox_ && this.enableThreadsRail_) {
      recordBoolean('NewTabPage.ThreadsRail.Shown', true);
    }

  }

  // For voice coherence: when error event is fired, this will run.
  onVoiceSearchError() {
    if (!this.showVoiceSearchOverlay_) {
      return;
    }
    this.hasVoiceSearchError = true;
  }

  protected onVoiceSearchRestart_() {
    this.hasVoiceSearchError = false;
    this.voiceSearchListening_ = true;
    this.voiceSearchReceivedSpeech_ = false;
    this.voiceSearchTranscript_ = '';
  }

  private computeShowCustomizeChromeText_(): boolean {
    if (this.showWallpaperSearchButton_) {
      return false;
    }
    return !this.showBackgroundImage_;
  }

  private computeBackgroundImageAttribution1_(): string {
    return this.theme_ && this.theme_.backgroundImageAttribution1 || '';
  }

  private computeBackgroundImageAttribution2_(): string {
    return this.theme_ && this.theme_.backgroundImageAttribution2 || '';
  }

  private computeBackgroundImageAttributionUrl_(): string {
    return this.theme_ && this.theme_.backgroundImageAttributionUrl || '';
  }

  private computeRealboxShown_(): boolean {
    // Do not show the realbox if the upload dialog is showing.
    return !!this.theme_ && !this.showLensUploadDialog_ &&
        !this.showComposebox_;
  }

  private computeModulesLoaded_(): boolean {
    return !loadTimeData.getBoolean('modulesEnabled') ||
        this.modulesLoadedStatus_ === ModuleLoadStatus.MODULE_LOAD_COMPLETE;
  }

  private onRealboxCanShowSecondarySideChanged_ = (e: MediaQueryListEvent) => {
    this.realboxCanShowSecondarySide = e.matches;
  };

  private onLazyRendered_() {
    // Integration tests use this attribute to determine when lazy load has
    // completed.
    document.documentElement.setAttribute('lazy-loaded', String(true));
    if (this.maybeRegisterCustomizeButtonHelpBubble_()) {
      this.pageHandler_.maybeTriggerAutomaticCustomizeChromePromo();
    }
    if (this.showWallpaperSearchButton_) {
      this.customizeButtonsHandler_.incrementWallpaperSearchButtonShownCount();
    }
  }

  private maybeRegisterCustomizeButtonHelpBubble_(): boolean {
    if (this.showCustomizeButton_ && !this.isFooterVisible_) {
      this.registerHelpBubble(
          CUSTOMIZE_CHROME_BUTTON_ELEMENT_ID,
          ['ntp-customize-buttons', '#customizeButton'], {fixed: true});
      return true;
    }
    return false;
  }

  protected onActionChipClick_(e: CustomEvent<ActionChipClickDetail>) {
    if (!this.composeboxEnabled) {
      // Enforce Composebox availability at the receiver even if an event
      // bypasses the Action Chips render gate.
      return;
    }
    const detail = e.detail;
    if (this.isUnsupportedSearchboxSurface_(detail.fuseboxAction)) {
      return;
    }
    this.pageHandler_.onContextualSearchIPHEngaged();
    if (this.hasRealboxOverride(detail.fuseboxAction)) {
      return;
    }
    this.openComposeboxForActionChip_(detail);
  }

  // The Composebox is the only searchbox surface supported for action chip
  // clicks; an explicit request for a different surface is a no-op. An action
  // with the override missing keeps the compatible open-Composebox behavior.
  private isUnsupportedSearchboxSurface_(action?: FuseboxAction): boolean {
    if (!action || action.searchboxOverride === null) {
      return false;
    }
    return action.searchboxOverride === SearchboxOverride.kUnspecified;
  }

  protected onOpenComposebox_(e: CustomEvent<ComposeboxState>) {
    this.openComposebox_(e.detail);
  }

  private async openComposeboxForActionChip_(detail: ActionChipClickDetail) {
    this.composeboxState_ = null;
    this.ensureComposeboxOpen_();
    await this.updateComplete;
    const composebox =
        this.shadowRoot?.querySelector<NtpComposeboxElement>('#composebox');
    if (composebox) {
      await composebox.handleFuseboxAction(detail);
    }
  }

  protected onContextMenuEntrypointClick_() {
    this.pageHandler_.onContextualSearchIPHEngaged();
  }

  protected openComposebox_(state: ComposeboxState) {
    this.composeboxState_ = state;
    this.ensureComposeboxOpen_();
  }

  private ensureComposeboxOpen_() {
    if (!this.showComposebox_) {
      this.showComposebox_ = true;
    }
    if (!this.wasComposeboxOpened_) {
      recordLoadDuration(
          'NewTabPage.Composebox.FromNTPLoadToSessionStart',
          WindowProxy.getInstance().now());
      this.wasComposeboxOpened_ = true;
    }
  }

  protected closeComposebox_() {
    this.showComposebox_ = false;
  }

  protected onScrimClick_() {
    if (this.showComposebox_) {
      this.onComposeboxOutsideClick_();
    }
    if (this.showLensUploadDialog_) {
      this.onCloseLensSearch_();
    }
    if (this.showVoiceSearchOverlay_) {
      const dialog = this.shadowRoot.querySelector<HTMLDialogElement>(
          '#voiceSearchDialog');
      if (dialog && dialog.open) {
        dialog.close();
      }
    }
    this.containerFocused_ = false;
  }

  protected onComposeboxOutsideClick_() {
    const composebox =
        this.shadowRoot.querySelector<NtpComposeboxElement>('#composebox');
    assert(composebox);
    this.onCloseComposebox_();
  }

  protected onComposeboxContextMenuOpened_() {
  }

  protected onCloseComposebox_() {
    if (!this.showComposebox_) {
      return;
    }
    const composebox =
        this.shadowRoot.querySelector<NtpComposeboxElement>('#composebox');
    assert(composebox);
    composebox.input = '';
    composebox.resetModes();
    if (this.ntpRealboxNextEnabled_) {
      composebox.closeDropdown();
    }
    this.closeComposebox_();
    this.logoColor_ = this.computeLogoColor_();
    this.singleColoredLogo_ = this.computeSingleColoredLogo_();
  }

  protected onOpenVoiceSearch_() {
    this.showVoiceSearchOverlay_ = true;
    // When the experiment is enabled, voice search metrics are logged directly
    // by the shared component instead of NTP to prevent duplicate emissions.
    if (!this.voiceSearchCoherenceAnySearchboxExperimentEnabled_) {
      recordVoiceAction(VoiceAction.ACTIVATE);
    }
  }

  protected onComposeVoiceSearchAction_(
      e: CustomEvent<{value: ComposeVoiceSearchAction}>) {
    if (this.voiceSearchCoherenceAnySearchboxExperimentEnabled_) {
      return;
    }
    switch (e.detail.value) {
      case ComposeVoiceSearchAction.ACTIVATE:
        recordVoiceAction(VoiceAction.ACTIVATE);
        break;
      case ComposeVoiceSearchAction.QUERY_SUBMITTED:
        recordVoiceAction(VoiceAction.QUERY_SUBMITTED);
        break;
      default:
        assertNotReached();
    }
  }

  protected onOpenLensSearch_() {
    this.showLensUploadDialog_ = true;
  }

  protected onCloseLensSearch_() {
    this.showLensUploadDialog_ = false;
  }

  protected onCustomizeClick_() {
    // Let side panel decide what page or section to show.
    this.selectedCustomizeDialogPage_ = null;
    this.setCustomizeChromeSidePanelVisible_(!this.showCustomize_);
    if (!this.showCustomize_) {
      this.customizeButtonsHandler_.incrementCustomizeChromeButtonOpenCount();
      recordCustomizeChromeOpen(NtpCustomizeChromeEntryPoint.CUSTOMIZE_BUTTON);
    }
  }

  protected computeShowWallpaperSearchButton_() {
    if (!this.wallpaperSearchButtonEnabled_) {
      return false;
    }

    switch (loadTimeData.getInteger('wallpaperSearchButtonHideCondition')) {
      case NtpWallpaperSearchButtonHideCondition.NONE:
        return true;
      case NtpWallpaperSearchButtonHideCondition.BACKGROUND_IMAGE_SET:
        return !this.showBackgroundImage_;
      case NtpWallpaperSearchButtonHideCondition.THEME_SET:
        return this.colorSourceIsBaseline && !this.showBackgroundImage_;
      default:
        return false;
    }
  }

  protected onWallpaperSearchClick_() {
    // Close the side panel if Wallpaper Search is open.
    if (this.showCustomize_ && this.showWallpaperSearch_) {
      this.selectedCustomizeDialogPage_ = null;
      this.setCustomizeChromeSidePanelVisible_(!this.showCustomize_);
      return;
    }

    // Open Wallpaper Search if the side panel is closed. Otherwise, navigate
    // the side panel to Wallpaper Search.
    this.selectedCustomizeDialogPage_ = CustomizeDialogPage.WALLPAPER_SEARCH;
    this.showWallpaperSearch_ = true;
    this.setCustomizeChromeSidePanelVisible_(this.showWallpaperSearch_);
    if (!this.showCustomize_) {
      this.customizeButtonsHandler_.incrementCustomizeChromeButtonOpenCount();
      recordCustomizeChromeOpen(
          NtpCustomizeChromeEntryPoint.WALLPAPER_SEARCH_BUTTON);
    }
  }

  onVoiceSearchOverlayClose() {
    const dialog =
        this.shadowRoot.querySelector<HTMLDialogElement>('#voiceSearchDialog');
    if (dialog && dialog.open) {
      return;
    }
    this.showVoiceSearchOverlay_ = false;
    this.hasVoiceSearchError = false;
    this.voiceSearchListening_ = false;
  }

  protected onVoicePermissionChanged_(
      e: CustomEvent<VoicePermissionPromptState>) {
    if (e.detail.isOpened) {
      this.voiceSearchListening_ = false;
    } else {
      this.voiceSearchListening_ =
          this.showVoiceSearchOverlay_ && !this.hasVoiceSearchError;
    }

    const audioAnimation =
        this.shadowRoot.querySelector<SearchAnimatedGlowElement>(
            '#voiceSearchGlow');
    if (audioAnimation) {
      if (e.detail.isOpened) {  // Permission prompt opened.
        audioAnimation.classList.add(PERMISSION_PROMPT_CSS_CLASS);
      } else {  // Permission prompt closed.
        audioAnimation.classList.remove(PERMISSION_PROMPT_CSS_CLASS);
      }
    }

    const voiceSearchElement =
        this.shadowRoot.querySelector<ComposeboxVoiceSearchElement>(
            '#voiceSearch');
    if (voiceSearchElement) {
      if (e.detail.isOpened) {  // Permission prompt opened.
        voiceSearchElement.classList.add(PERMISSION_PROMPT_CSS_CLASS);
      } else {  // Permission prompt closed.
        voiceSearchElement.classList.remove(PERMISSION_PROMPT_CSS_CLASS);
      }
    }
  }

  protected onVoiceSearchCancel_() {
    const dialog =
        this.shadowRoot.querySelector<HTMLDialogElement>('#voiceSearchDialog');
    if (dialog) {
      dialog.close();
    }
    this.onVoiceSearchOverlayClose();
  }

  protected onVoiceSearchTranscriptUpdate_(e: CustomEvent<string>) {
    this.voiceSearchTranscript_ = e.detail;
  }

  protected onVoiceSearchSpeechReceived_() {
    this.voiceSearchReceivedSpeech_ = true;
  }

  protected showVoiceSearchGlow_(): boolean {
    if (this.hasVoiceSearchError) {
      return false;
    }
    // Audio wave is rendered for all 4 Realbox experiment arms (which have
    // helper text enabled) and the legacy 'No Live Transcription' arm.
    return !this.voiceSearchCoherenceSearchboxWithLiveTranscriptionEnabled_ ||
        this.voiceSearchCoherenceRealboxHelperTextEnabled_;
  }

  protected onVoiceSearchDialogClick_(e: MouseEvent) {
    const dialog = e.currentTarget as HTMLDialogElement;
    if (e.target === dialog) {
      dialog.close();
    }
  }

  protected onVoiceSearchFinalResult_(e: CustomEvent<string>) {
    // Receiving a final result also closes the voice search dialog.
    const dialog =
        this.shadowRoot.querySelector<HTMLDialogElement>('#voiceSearchDialog');
    if (dialog) {
      dialog.close();
    }
    this.onVoiceSearchOverlayClose();

    const searchParams = new URLSearchParams();
    searchParams.append('q', e.detail);
    searchParams.append('gs_ivs', '1');
    searchParams.append('sourceid', 'chrome');
    const queryUrl =
        new URL('/search', loadTimeData.getString('googleBaseUrl'));
    queryUrl.search = searchParams.toString();
    WindowProxy.getInstance().navigate(queryUrl.href);
  }

  private onWindowKeydown_(e: KeyboardEvent) {
    if (!this.isConnected) {
      return;
    }
    if (e.key === 'Escape' && this.showComposebox_) {
      const composebox =
          this.shadowRoot.querySelector<NtpComposeboxElement>('#composebox');
      if (composebox) {
        composebox.handleEscapeKeyLogic();
        e.preventDefault();
        return;
      }
    }
  }

  private rgbaOrInherit_(skColor: SkColor|null): string {
    return skColor ? skColorToRgba(skColor) : 'inherit';
  }

  private computeShowBackgroundImage_(): boolean {
    return !!this.theme_ && !!this.theme_.backgroundImage;
  }

  private onShowBackgroundImageChange_() {
    this.backgroundManager_.setShowBackgroundImage(this.showBackgroundImage_);
  }

  private onThemeChange_() {
    if (this.theme_) {
      const glass = this.theme_.noventraGlassEnabled &&
          !this.theme_.backgroundImage;
      this.toggleAttribute('noventra-glass', glass);
      // Tint and transparency arrive together from the native contents view.
      // The ColorProvider CSS palette remains opaque for foreground contrast.
      this.backgroundManager_.setBackgroundColor(this.theme_.backgroundColor);
      this.style.setProperty(
          '--color-new-tab-page-attribution-foreground',
          this.rgbaOrInherit_(this.theme_.textColor));
      this.style.setProperty(
          '--color-new-tab-page-most-visited-foreground',
          this.rgbaOrInherit_(this.theme_.textColor));
    }
    this.updateBackgroundImagePath_();
  }

  private scheduleNoventraForegroundReport_() {
    if (!this.isConnected || this.noventraForegroundFrame_ !== null) {
      return;
    }
    this.noventraForegroundFrame_ = requestAnimationFrame(() => {
      this.noventraForegroundFrame_ = null;
      this.measureNoventraForeground_();
    });
  }

  private measureNoventraForeground_() {
    const roots = new Set<ShadowRoot>([this.shadowRoot]);
    const elements = new Set<Element>();
    const search = this.shadowRoot.querySelector('#noventraSearch');
    if (!this.lazyRender_ || !this.theme_?.noventraGlassEnabled ||
        this.theme_.backgroundImage || document.visibilityState !== 'visible' ||
        innerWidth <= 0 || innerHeight <= 0 ||
        (window.visualViewport && window.visualViewport.scale !== 1) ||
        search?.hasAttribute('dropdown-is-visible') || this.showComposebox_ ||
        this.showLensUploadDialog_ || this.showVoiceSearchOverlay_) {
      this.updateNoventraForegroundObservers_(roots, elements);
      this.sendNoventraForegroundReport_([], false);
      return;
    }
    let complete = !!search;
    const bounds: NoventraForegroundRect[] = [];
    // Include actual descendant overflow (including shadow DOM), rather than
    // guessing the search/shortcuts dimensions from CSS. Top-layer UI cannot
    // be bounded by its host, so it conservatively disables material sharing.
    for (const root of this.shadowRoot.querySelectorAll<HTMLElement>(
             '#content > *')) {
      elements.add(root);
      let left = innerWidth;
      let top = innerHeight;
      let right = 0;
      let bottom = 0;
      const visit = (element: Element) => {
        if (element.matches('dialog[open], :popover-open')) {
          complete = false;
        }
        const style = getComputedStyle(element);
        if (style.display === 'none' || style.visibility === 'hidden') {
          return;
        }
        const rect = element.getBoundingClientRect();
        if (rect.width > 0 && rect.height > 0) {
          left = Math.min(left, rect.left);
          top = Math.min(top, rect.top);
          right = Math.max(right, rect.right);
          bottom = Math.max(bottom, rect.bottom);
        }
        if (element.shadowRoot) {
          roots.add(element.shadowRoot);
          Array.from(element.shadowRoot.children).forEach(visit);
        }
        Array.from(element.children).forEach(visit);
      };
      visit(root);
      left = Math.max(0, Math.floor(left));
      top = Math.max(0, Math.floor(top));
      right = Math.min(innerWidth, Math.ceil(right));
      bottom = Math.min(innerHeight, Math.ceil(bottom));
      if (right > left && bottom > top) {
        bounds.push({
          x: left / innerWidth,
          y: top / innerHeight,
          width: (right - left) / innerWidth,
          height: (bottom - top) / innerHeight,
        });
      }
    }
    this.updateNoventraForegroundObservers_(roots, elements);
    this.sendNoventraForegroundReport_(bounds, complete && bounds.length > 0);
  }

  private updateNoventraForegroundObservers_(
      roots: Set<ShadowRoot>, elements: Set<Element>) {
    let rootsChanged = false;
    for (const root of this.noventraForegroundRoots_) {
      if (!roots.has(root)) {
        root.removeEventListener(
            'scroll', this.noventraForegroundScroll_, true);
        this.noventraForegroundRoots_.delete(root);
        rootsChanged = true;
      }
    }
    for (const root of roots) {
      if (!this.noventraForegroundRoots_.has(root)) {
        root.addEventListener('scroll', this.noventraForegroundScroll_, true);
        this.noventraForegroundRoots_.add(root);
        rootsChanged = true;
      }
    }
    if (rootsChanged) {
      this.noventraForegroundMutations_.disconnect();
      for (const root of this.noventraForegroundRoots_) {
        this.noventraForegroundMutations_.observe(root, {
          subtree: true,
          childList: true,
          characterData: true,
          attributes: true,
          attributeFilter:
              ['class', 'style', 'hidden', 'open', 'dropdown-is-visible'],
        });
      }
    }
    for (const element of this.noventraForegroundElements_) {
      if (!elements.has(element)) {
        this.noventraForegroundResize_.unobserve(element);
        this.noventraForegroundElements_.delete(element);
      }
    }
    for (const element of elements) {
      if (!this.noventraForegroundElements_.has(element)) {
        this.noventraForegroundResize_.observe(element);
        this.noventraForegroundElements_.add(element);
      }
    }
  }

  private sendNoventraForegroundReport_(
      bounds: NoventraForegroundRect[], complete: boolean) {
    const width = complete ? innerWidth : 0;
    const height = complete ? innerHeight : 0;
    const ratio = complete ? devicePixelRatio : 0;
    if (!complete) {
      bounds = [];
    }
    const key = JSON.stringify([bounds, width, height, ratio, complete]);
    if (key === this.noventraForegroundReport_) {
      return;
    }
    this.noventraForegroundReport_ = key;
    this.pageHandler_
        .setNoventraForegroundBounds(bounds, width, height, ratio, complete)
        .then(({accepted}) => {
          // Native resize can precede its renderer viewport update. Retry a
          // rejected stale snapshot, with a bound so invalid state stays
          // backed.
          if (!accepted && complete && this.isConnected &&
              this.noventraForegroundReport_ === key &&
              this.noventraForegroundRetries_ < 2) {
            ++this.noventraForegroundRetries_;
            this.noventraForegroundReport_ = '';
            this.scheduleNoventraForegroundReport_();
          } else if (accepted) {
            this.noventraForegroundRetries_ = 0;
          }
        });
  }

  private onThemeLoaded_(theme: Theme) {
    recordSparseValueWithPersistentHash(
        'NewTabPage.Collections.IdOnLoad',
        theme.backgroundImageCollectionId ?? '');

    if (!theme.backgroundImage) {
      recordEnumeration(
          'NewTabPage.BackgroundImageSource', NtpBackgroundImageSource.kNoImage,
          NtpBackgroundImageSource.MAX_VALUE + 1);
    } else {
      recordEnumeration(
          'NewTabPage.BackgroundImageSource', theme.backgroundImage.imageSource,
          NtpBackgroundImageSource.MAX_VALUE + 1);
    }
  }

  private onModulesLoadedChange_() {
    if (this.modulesLoaded_ &&
        loadTimeData.getBoolean('modulesEnabled')) {
      recordLoadDuration(
          'NewTabPage.Modules.ShownTime', WindowProxy.getInstance().now());
    }
  }

  /**
   * Set the #backgroundImage |path| only when different and non-empty. Reset
   * the customize dialog background selection if the dialog is closed.
   *
   * The ntp-untrusted-iframe |path| is set directly. When using a data binding
   * instead, the quick updates to the |path| result in iframe loading an error
   * page.
   */
  private updateBackgroundImagePath_() {
    const backgroundImage = this.theme_ && this.theme_.backgroundImage;
    if (!backgroundImage) {
      return;
    }

    this.backgroundManager_.setBackgroundImage(backgroundImage);

    if (this.wallpaperSearchButtonAnimationEnabled_ &&
            backgroundImage.imageSource ===
                NtpBackgroundImageSource.kWallpaperSearch ||
        backgroundImage.imageSource ===
            NtpBackgroundImageSource.kWallpaperSearchInspiration) {
      this.wallpaperSearchButtonAnimationEnabled_ = false;
    }
  }

  private computeColorSourceIsBaseline(): boolean {
    return !!this.theme_ && this.theme_.isBaseline;
  }

  private computeLogoColor_(): SkColor|null {
    if (!this.theme_) {
      return null;
    }

    return this.theme_.logoColor ||
        (this.theme_.isDark ? hexColorToSkColor('#ffffff') : null);
  }

  private computeSingleColoredLogo_(): boolean {
    return !!this.theme_ && (!!this.theme_.logoColor || this.theme_.isDark);
  }

  protected onModulesLoaded_(e: CustomEvent<number|null>) {
    this.modulesLoadedStatus_ = e.detail ?
        ModuleLoadStatus.MODULE_LOAD_COMPLETE :
        ModuleLoadStatus.MODULE_LOAD_NOT_ATTEMPTED;
  }

  protected computeShowBrowserPromo_(): boolean {
    return !this.modulesEnabled_ ||
        (this.modulesLoadedStatus_ !==
             ModuleLoadStatus.MODULE_LOAD_IN_PROGRESS &&
         !this.modulesShownToUser);
  }

  protected recordBrowserPromoMetrics_() {
    if (!this.showBrowserPromo_) {
      recordShowBrowserPromosResult(ShowNtpPromosResult.kNotShownDueToPolicy);
      return;
    }

    switch (this.browserPromoType_) {
      case 'disabled':
        recordShowBrowserPromosResult(ShowNtpPromosResult.kNotShownDueToPolicy);
        break;
      case 'empty':
        recordShowBrowserPromosResult(ShowNtpPromosResult.kNotShownNoPromos);
        break;
      case 'simple':
        recordShowBrowserPromosResult(ShowNtpPromosResult.kShown);
        break;
      default:
        break;
    }
  }

  protected onCustomizeModule_() {
    this.showCustomize_ = true;
    this.selectedCustomizeDialogPage_ = CustomizeDialogPage.MODULES;
    recordCustomizeChromeOpen(NtpCustomizeChromeEntryPoint.MODULE);
    this.setCustomizeChromeSidePanelVisible_(this.showCustomize_);
  }

  private setCustomizeChromeSidePanelVisible_(visible: boolean) {
    let section: CustomizeChromeSection = CustomizeChromeSection.kUnspecified;
    switch (this.selectedCustomizeDialogPage_) {
      case CustomizeDialogPage.BACKGROUNDS:
      case CustomizeDialogPage.THEMES:
        section = CustomizeChromeSection.kAppearance;
        break;
      case CustomizeDialogPage.SHORTCUTS:
        section = CustomizeChromeSection.kShortcuts;
        break;
      case CustomizeDialogPage.MODULES:
        section = CustomizeChromeSection.kModules;
        break;
      case CustomizeDialogPage.WALLPAPER_SEARCH:
        section = CustomizeChromeSection.kWallpaperSearch;
        break;
      default:
        break;
    }
    this.customizeButtonsHandler_.setCustomizeChromeSidePanelVisible(
        visible, section, SidePanelOpenTrigger.kNewTabPage);
  }

  private printPerformanceDatum_(
      name: string, time: number, auxTime: number = 0) {
    if (!this.shouldPrintPerformance_) {
      return;
    }

    console.info(
        !auxTime ? `${name}: ${time}` : `${name}: ${time} (${auxTime})`);
  }

  /**
   * Prints performance measurements to the console. Also, installs  performance
   * observer to continuously print performance measurements after.
   */
  private printPerformance_() {
    if (!this.shouldPrintPerformance_) {
      return;
    }
    const entryTypes = ['paint', 'measure'];
    const log = (entry: PerformanceEntry) => {
      this.printPerformanceDatum_(
          entry.name, entry.duration ? entry.duration : entry.startTime,
          entry.duration && entry.startTime ? entry.startTime : 0);
    };
    const observer = new PerformanceObserver(list => {
      list.getEntries().forEach((entry) => {
        log(entry);
      });
    });
    observer.observe({entryTypes: entryTypes});
    performance.getEntries().forEach((entry) => {
      if (!entryTypes.includes(entry.entryType)) {
        return;
      }
      log(entry);
    });
  }

  protected onWebstoreToastButtonClick_() {
    window.location.assign(
        `https://chrome.9oo91e.qjz9zk/webstore/category/collection/chrome_color_themes?hl=${
            window.navigator.language}`);
  }

  private onWindowClick_(e: Event) {
    if (e.composedPath() && e.composedPath()[0] === $$(this, '#content')) {
      recordClick(NtpElement.BACKGROUND);
      return;
    }
    for (const target of e.composedPath()) {
      switch (target) {
        case $$(this, 'ntp-logo'):
          recordClick(NtpElement.LOGO);
          return;
        case $$(this, 'ntp-searchbox'):
          recordClick(NtpElement.REALBOX);
          return;
        case $$(this, 'ntp-action-chips'):
          recordClick(NtpElement.ACTION_CHIPS);
          return;
        case $$(this, 'cr-most-visited'):
          recordClick(NtpElement.MOST_VISITED);
          return;
        case $$(this, '#modules'):
          recordClick(NtpElement.MODULE);
          return;
        case $$(this, '#threadsRail'):
          recordClick(NtpElement.THREADS_RAIL);
          return;
        default:
          break;
      }
    }

    const customizeButtonsElement =
        this.shadowRoot.querySelector('ntp-customize-buttons');
    if (customizeButtonsElement) {
      for (const target of e.composedPath()) {
        switch (target) {
          case $$(customizeButtonsElement, '#customizeButton'):
            recordClick(NtpElement.CUSTOMIZE_BUTTON);
            return;
          case $$(customizeButtonsElement, '#wallpaperSearchButton'):
            recordClick(NtpElement.WALLPAPER_SEARCH_BUTTON);
            return;
          default:
            break;
        }
      }
    }

    recordClick(NtpElement.OTHER);
  }

  protected isThemeDark_(): boolean {
    return !!this.theme_ && this.theme_.isDark;
  }

  protected showActionChipsBackground_(): boolean {
    return !!this.theme_ &&
        (!!this.theme_.backgroundImage || !this.theme_.isGm3);
  }

  protected showThemeAttribution_(): boolean {
    return !!this.theme_?.backgroundImage?.attributionUrl;
  }

  protected onRealboxHadSecondarySideChanged_(
      e: CustomEvent<{value: boolean}>) {
    this.realboxHadSecondarySide = e.detail.value;
  }

  protected onSearchboxContainerFocusin_(e: FocusEvent) {
    if (this.ntpRealboxNextEnabled_) {
      const isHelpBubble = e.composedPath().some(
          el => (el as HTMLElement)?.tagName === 'HELP-BUBBLE');
      if (!isHelpBubble) {
        this.containerFocused_ = true;
      }
    }
  }

  protected onSearchboxContainerFocusout_() {
    if (this.ntpRealboxNextEnabled_) {
      this.containerFocused_ =
          this.shadowRoot.getElementById('searchboxContainer')!.matches(
              ':focus-within');
    }
  }

  protected onModulesShownToUserChanged_(e: CustomEvent<{value: boolean}>) {
    this.modulesShownToUser = e.detail.value;
  }

  private async initializeContextMenuAnimationState_() {
    if (this.ntpRealboxNextEnabled_) {
      let canShow = true;
      if (this.contextMenuAnimationLimitingEnabled_) {
        const {canShow: allowed} =
            await this.pageHandler_.canShowRealboxContextMenuAnimation();
        canShow = allowed;
      }
      this.realboxContextMenuAnimationAllowed_ = canShow;
      if (canShow) {
        if (this.energyEffectAnimationEnabled_) {
          this.contextMenuGlifAnimationState_ = GlifAnimationState.STARTED;
          this.pageHandler_.recordRealboxContextMenuAnimationImpression(true);
        } else {
          const isSpinnerEligible =
              this.ntpNextFeaturesEnabled_ && this.isActionChipsVisible_;
          this.contextMenuGlifAnimationState_ = isSpinnerEligible ?
              GlifAnimationState.SPINNER_ONLY :
              GlifAnimationState.INELIGIBLE;
        }
      } else {
        this.contextMenuGlifAnimationState_ = GlifAnimationState.INELIGIBLE;
        this.pageHandler_.recordRealboxContextMenuAnimationImpression(false);
      }
    } else {
      this.realboxContextMenuAnimationAllowed_ = false;
      this.contextMenuGlifAnimationState_ = GlifAnimationState.INELIGIBLE;
    }
  }

  protected onActionChipsRetrievalStateChanged_(
      e: CustomEvent<{state: ActionChipsRetrievalState}>) {
    if (!this.realboxContextMenuAnimationAllowed_) {
      this.contextMenuGlifAnimationState_ = GlifAnimationState.INELIGIBLE;
      return;
    }
    if (this.energyEffectAnimationEnabled_) {
      return;
    }
    const state = e.detail.state;
    // Mapping of ActionChipsRetrievalState => GlifAnimationState:
    // REQUESTED => SPINNER_ONLY
    // UPDATED => STARTED (or FINISHED if cr_context_menu_entrypoint sets it)
    // To avoid going back (or continuing) GlifAnimationState.STARTED, we stop
    // updating the field when the current state is STARTED or FINISHED.
    // There are a few cases to consider:
    // - IsActionChipsVisible_ is false (and remains so): no event from the
    //   action chips element, and thus the animation state remains INELIGIBLE.
    // - IsActionChipsVisible_ is false and later becomes true: the change
    //   triggers the rendering of the action chips element, and this in turn
    //   causes an event with ActionChipsRetrievalState.REQUESTED to be fired.
    //   After some time, an event with ActionChipsRetrievalState.UPDATED will
    //   fire, and this starts the animation.
    // - IsActionChipsVisible_ is true from the beginning: Same as above.
    if ([GlifAnimationState.STARTED, GlifAnimationState.FINISHED].every(
            s => s !== this.contextMenuGlifAnimationState_)) {
      if (state === ActionChipsRetrievalState.REQUESTED) {
        this.contextMenuGlifAnimationState_ = GlifAnimationState.SPINNER_ONLY;
      } else if (state === ActionChipsRetrievalState.UPDATED) {
        this.contextMenuGlifAnimationState_ = GlifAnimationState.STARTED;
        this.pageHandler_.recordRealboxContextMenuAnimationImpression(true);
      }
    }
  }

  /**
   * Called whenever an auto-removed feature is being processed and the undo
   * toast needs to be shown. This will queue up the toast in the pending FIFO
   * list and then call the processing function.
   *
   * @param undoToastContext - An event that contains the undo toast message and
   *                           the undo callback function.
   */
  protected onMostVisitedAutoRemoved_(
      undoToastContext: CustomEvent<{message: string, undo: () => void}>) {
    this.showUndoToast_(undoToastContext);
  }

  protected onModulesAutoRemoved_(
      undoToastContext: CustomEvent<{message: string, undo: () => void}>) {
    this.showUndoToast_(undoToastContext);
  }

  protected onActionChipsDisabled_(
      undoToastContext: CustomEvent<{message: string, undo: () => void}>) {
    this.showUndoToast_(undoToastContext);
  }

  protected showUndoToast_(
      undoToastContext: CustomEvent<{message: string, undo: () => void}>) {
    this.pendingUndoToasts_.push(undoToastContext.detail);
    this.processPendingUndoToasts_();
  }

  /**
   * Called whenever the pending toasts need to be processed. This is called
   * whenever a new toast is added to the pending list through an auto-removal
   * event, or when the user clicks on the undo button in the toast.
   *
   * In case the undo toast is already open, then it's a no-op to avoid showing
   * multiple toasts at the same time. Otherwise, the first pending toast is
   * popped and shown.
   */
  private processPendingUndoToasts_() {}

  /**
   * Processes an auto-removal undo click. It will hide the toast, call the
   * undo callback, and call the processing function to handle the next queued
   * toast (if any).
   */
  protected onUndoButtonClick_() {}

  private onShowComposeboxChange_() {
    const notSelector =
        COMPOSEBOX_INERT_ALLOWLIST.map(s => `:not(${s})`).join('');
    const blockedElements = this.shadowRoot.querySelectorAll<HTMLElement>(
        `#content > ${notSelector}`);
    blockedElements.forEach(element => {
      if (this.showComposebox_) {
        element.setAttribute('inert', '');
      } else {
        element.removeAttribute('inert');
      }
    });
  }

  private hasRealboxOverride(action?: FuseboxAction): boolean {
    return !!action && action?.searchboxOverride === SearchboxOverride.kRealbox;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'ntp-app': AppElement;
  }
}

customElements.define(AppElement.is, AppElement);
