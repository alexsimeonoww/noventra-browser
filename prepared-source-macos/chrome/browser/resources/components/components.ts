// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import '/strings.m.js';

import {assert} from 'chrome://resources/js/assert.js';
import {addWebUiListener, sendWithPromise} from 'chrome://resources/js/cr.js';
import {loadTimeData} from 'chrome://resources/js/load_time_data.js';
import {isChromeOS} from 'chrome://resources/js/platform.js';
import {getRequiredElement} from 'chrome://resources/js/util.js';
import {render} from 'chrome://resources/lit/v3_0/lit.rollup.js';

import {getHtml} from './component.html.js';

interface Component {
  id: string;
  name: string;
  status: string;
  version: string;
  widevine?: boolean;
}

export interface ComponentsData {
  components: Component[];
  showUninstallButton?: boolean;
  widevineAvailable?: boolean;
  widevineDownloadsEnabled?: boolean;
  widevineCanChange?: boolean;
  widevineDownloadsBusy?: boolean;
  widevineDownloadsError?: boolean;
  widevineRestartRequired?: boolean;
  widevineRestartSupported?: boolean;
}

/**
 * The latest page model, including component status and download controls.
 * Keep Lit-owned text and attributes synchronized through this model.
 */
let currentComponentsData: ComponentsData|null = null;
let widevineRefreshTimer: number|undefined;

function refreshWidevineRestartStatus(attempts: number) {
  sendWithPromise<ComponentsData>('requestComponentsData').then(data => {
    if (currentComponentsData) {
      currentComponentsData.widevineRestartRequired = data.widevineRestartRequired;
      renderTemplate(currentComponentsData);
    }
    if (!data.widevineRestartRequired && attempts > 0) {
      widevineRefreshTimer = window.setTimeout(
          () => refreshWidevineRestartStatus(attempts - 1), 1000);
    }
  });
}

/**
 * Takes the |componentsData| input argument which represents data about the
 * currently installed components and populates the Lit HTML template with
 * that data. It expects an object structure like the above.
 * @param componentsData Detailed info about installed components.
 *      Same expected format as returnComponentsData().
 */
function renderTemplate(componentsData: ComponentsData) {
  render(getHtml(componentsData), getRequiredElement('component-placeholder'));
}

/**
 * Asks the C++ ComponentsDOMHandler to get details about the installed
 * components.
 */
function requestComponentsData() {
  return sendWithPromise<ComponentsData>('requestComponentsData')
      .then(returnComponentsData);
}

/**
 * Called by the WebUI to re-populate the page with data representing the
 * current state of installed components. The componentsData will also be
 * stored in currentComponentsData to be available to JS for testing purposes.
 * @param componentsData Detailed info about installed components.
 */
function returnComponentsData(componentsData: ComponentsData) {
  const bodyContainer = getRequiredElement('body-container');
  const body = document.body;

  body.className = '';

  componentsData.components.sort((a, b) => {
    const nameA = a.name || a.id;
    const nameB = b.name || b.id;
    return nameA.localeCompare(nameB) || a.id.localeCompare(b.id);
  });

  // Initialize |currentComponentsData|, which can also be updated in
  // onComponentEvent() later.
  currentComponentsData = componentsData;
  componentsData.showUninstallButton =
      loadTimeData.getBoolean('showUninstallButton');

  renderTemplate(componentsData);

  const downloads = document.querySelector<HTMLInputElement>(
      '#widevine-downloads');
  if (downloads) {
    downloads.onchange = () => {
      const wasEnabled = !!componentsData.widevineDownloadsEnabled;
      const enabled = downloads.checked;
      componentsData.widevineDownloadsEnabled = enabled;
      componentsData.widevineDownloadsBusy = true;
      componentsData.widevineDownloadsError = false;
      renderTemplate(componentsData);
      sendWithPromise<boolean>('setWidevineDownloadsEnabled', enabled)
          .then(requestComponentsData)
          .catch(() => {
            componentsData.widevineDownloadsEnabled = wasEnabled;
            componentsData.widevineDownloadsBusy = false;
            componentsData.widevineDownloadsError = true;
            renderTemplate(componentsData);
          });
    };
  }

  // Add handlers to dynamically created HTML elements.
  const links =
      document.body.querySelectorAll<HTMLButtonElement>('.button-check-update');
  for (const link of links) {
    link.onclick = function(e) {
      handleCheckUpdate(link);
      e.preventDefault();
    };
  }

  const uninstallButtons =
      document.body.querySelectorAll<HTMLButtonElement>('.button-uninstall');
  for (const btn of uninstallButtons) {
    btn.onclick = function(e) {
      handleUninstall(btn);
      e.preventDefault();
    };
  }

  // Disable some controls for Guest mode in ChromeOS.
  if (isChromeOS && loadTimeData.getBoolean('isGuest')) {
    document.body.querySelectorAll<HTMLButtonElement>('[guest-disabled]')
        .forEach(function(element) {
          element.disabled = true;
        });
  }

  bodyContainer.style.visibility = 'visible';
  body.className = 'show-tmi-mode-initial';
}

interface ComponentEvent {
  event: string;
  id?: string;
  version?: string;
}

/**
 * Listener called when state of component updater service changes.
 * @param event Contains event and component ID. Component ID is
 *     optional.
 */
function onComponentEvent(event: ComponentEvent) {
  if (!event.id) {
    return;
  }

  const id = event.id;

  assert(currentComponentsData);
  const filteredComponents =
      currentComponentsData.components.filter(function(entry) {
        return entry.id === id;
      });

  // A component may be added from another page so the status and version
  // should only be updated if the component is listed on this page.
  if (filteredComponents.length === 0) {
    return;
  }

  if (currentComponentsData.widevineRestartSupported && event.id ===
      'oimompecagnajdejgnnjijobebaeigek') {
    window.clearTimeout(widevineRefreshTimer);
    widevineRefreshTimer = window.setTimeout(
        () => refreshWidevineRestartStatus(5), 250);
  }

  const component = filteredComponents[0];
  assert(component);

  const status = event.event;
  component.status = status;

  if (event.version) {
    const version = event.version;
    component.version = version;
  }
  renderTemplate(currentComponentsData);
}

/**
 * Handles an 'enable' or 'disable' button getting clicked.
 * @param node The HTML element representing the component being checked for
 *     update.
 */
function handleCheckUpdate(node: HTMLElement) {
  assert(currentComponentsData);
  const component = currentComponentsData.components.find(
      entry => entry.id === node.id);
  if (!component) {
    return;
  }
  component.status = loadTimeData.getString('checkingLabel');
  renderTemplate(currentComponentsData);

  // Tell the C++ ComponentssDOMHandler to check for update.
  chrome.send('checkUpdate', [node.id]);
}

/**
 * Handles an 'uninstall' button getting clicked.
 * @param node The HTML element representing the component being uninstalled.
 */
function handleUninstall(node: HTMLElement) {
  node.setAttribute('disabled', 'true');

  // Tell the C++ ComponentsDOMHandler to uninstall the component.
  chrome.send('uninstallComponent', [node.id]);
}

// Get data and have it displayed upon loading.
document.addEventListener('DOMContentLoaded', function() {
  addWebUiListener('component-event', onComponentEvent);
  requestComponentsData();
});
