# Extension smoke fixture

A local, unpacked Manifest V3 extension for a disposable browser profile. It tests a storage round trip, service-worker messaging after an idle delay, and querying the extension page through the tabs API. It has no host permissions or network calls.

From the repository root:

```sh
python3 tests/noventra/extension_smoke.py --browser /absolute/path/to/browser --output work/extension-smoke/results.json
```

The test starts a headless browser with a temporary profile, opens the extension document, checks its result, and shuts down the browser. The output records the exact browser version and scope. The idle delay does not prove that Chrome suspended the worker.

Passing does not certify toolbar popup interaction, Chrome Web Store installation, native messaging, arbitrary extensions, or Manifest V2. Run this test against both the baseline and the eventual Noventra executable.
