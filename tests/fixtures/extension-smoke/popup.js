(async () => {
  try {
    await chrome.storage.local.set({noventraSmokeValue: "round-trip"});
    const stored = await chrome.storage.local.get("noventraSmokeValue");
    if (stored.noventraSmokeValue !== "round-trip") throw Error("Storage round trip failed");
    const reply = await chrome.runtime.sendMessage({type: "noventra-smoke-ping"});
    if (!reply?.worker || reply.manifestVersion !== 3) throw Error("Service worker messaging failed");
    const tabs = await chrome.tabs.query({});
    if (!tabs.some(tab => tab.url === location.href)) throw Error("Tabs API did not return this extension page");
    document.getElementById("result").textContent = "PASS: storage, service-worker messaging, tabs API";
    document.title = "Extension checks passed";
  } catch (error) {
    document.getElementById("result").textContent = String(error);
    document.title = "Extension checks failed: " + error.message;
  }
})();
