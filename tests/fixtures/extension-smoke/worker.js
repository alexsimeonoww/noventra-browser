chrome.runtime.onMessage.addListener((message, _sender, respond) => {
  if (message.type === "noventra-smoke-ping") {
    respond({worker: true, manifestVersion: chrome.runtime.getManifest().manifest_version});
  }
});
