// Copyright 2025 The Helium Authors
// You can use, redistribute, and/or modify this source code under
// the terms of the GPL-3.0 license that can be found in the LICENSE file.

(() => {
  const PROMO_POPUP_SELECTOR = 'header > [aria-labelledby=promo-header]';
  // 276336 is the store's logging ID for the "Install Chrome" button in the
  // "Switch to Chrome" banner. It's the same in every locale, but it's an
  // internal value that the store may change at any time, so update it here
  // if the Chrome eligibility override in chrome-webstore-main.js fails and
  // the banner comes back.
  const SWITCH_NAG_SELECTOR =
      'main section div:has(> div > div > button[jslog^="276336;"])';
  const CHROME_NAME_REGEX = /\b(?:Google\s)?Chrome\b/;

  const removeChromeNags = () => {
    document.querySelector(PROMO_POPUP_SELECTOR)?.remove();
    document.querySelector(SWITCH_NAG_SELECTOR)?.remove();
  };

  const replaceDownloadButtonText = (el) => {
    if (el.nodeType === Node.TEXT_NODE) {
      const replacement = el.nodeValue.replace(CHROME_NAME_REGEX, 'Noventra');
      if (replacement === el.nodeValue) {
        return false;
      }
      el.nodeValue = replacement;
      return true;
    }
    return Array.from(el.childNodes).some(replaceDownloadButtonText);
  };

  const getDownloadButton = (section) => {
    return Array.from(section?.querySelectorAll('button') ?? [])
        .find((button) => button.textContent.includes('Chrome'));
  };

  const changeDownloadButtonText = () => {
    const mainTitles = document.querySelectorAll('main section h1');

    for (const mainTitleEl of mainTitles) {
      const downloadButton =
          getDownloadButton(mainTitleEl.parentElement?.parentElement);
      if (downloadButton && replaceDownloadButtonText(downloadButton)) {
        break;
      }
    }
  };

  const handleChanges = () => {
    // Removing the nags before the page has fully loaded can happen before
    // the page's controllers attach to them, so wait until it's complete.
    if (document.readyState === 'complete') {
      removeChromeNags();
    }
    changeDownloadButtonText();
  };

  window.addEventListener('load', handleChanges, {once: true});
  const observer = new MutationObserver(handleChanges);
  observer.observe(document, {
    childList: true,
    characterData: true,
    subtree: true,
  });
})();
