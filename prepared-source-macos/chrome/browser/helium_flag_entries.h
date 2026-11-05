// Copyright 2025 The Helium Authors
// You can use, redistribute, and/or modify this source code under
// the terms of the GPL-3.0 license that can be found in the LICENSE file.

#ifndef CHROME_BROWSER_HELIUM_FLAG_ENTRIES_H_
#define CHROME_BROWSER_HELIUM_FLAG_ENTRIES_H_
{kHeliumFlagFormattingSentinel, "", "", flags_ui::kFlagInfrastructure,
 SINGLE_VALUE_TYPE("")},
    // core
    {helium::kEnergySaverFrameRateCommandLine,
     "Maximum frame rate for Energy Saver",
     "Configures the frame rate the browser is throttled to when Energy Saver "
     "is enabled. Noventra flag.",
     kOsDesktop, MULTI_VALUE_TYPE(helium::kEnergySaverFrameRateChoices)},
    {"bookmark-open-all-confirmation", "Bookmark open-all confirmation",
     "Controls whether Noventra asks for confirmation before opening 15 or more "
     "bookmarks at once. Noventra flag.",
     kOsDesktop, FEATURE_VALUE_TYPE(features::kBookmarkOpenAllConfirmation)},
    {helium::kChannelCommandLine, "Update channel",
     "Selects which update channel to use for update checking. Noventra flag.",
     kOsAll, MULTI_VALUE_TYPE(helium::kChannelChoices)},
    {helium::kUpdateServerUrlCommandLine, "Update server URL",
     "Overrides the default URL for fetching updates. Should be used only for "
     "testing. Noventra flag.",
     kOsDesktop, STRING_VALUE_TYPE(helium::kUpdateServerUrlCommandLine, "")},
    {helium::kMiddleClickAutoscrollCommandLine, "Middle Click Autoscroll",
     "Enables autoscroll on middle click. Noventra flag, Chromium feature.",
     kOsDesktop,
     FEATURE_VALUE_TYPE(blink::features::kHeliumMiddleClickAutoscroll)},
    {helium::kHeliumNoiseCommandLine, "Noventra Noise",
     "Noventra's anti-fingerprinting functionality. Adds insignificant noise to "
     "various features to deceive fingerprinting scripts. Enabled by default, "
     "must be enabled for other anti-fingerprinting features to work. Noventra "
     "flag.",
     kOsAll, FEATURE_VALUE_TYPE(blink::features::kHeliumNoise)},
    {helium::kHeliumNoiseCanvasCommandLine,
     "[Noventra Noise] Canvas pixel noising",
     "Adds insignificant noise to canvas pixels during readback to deceive "
     "canvas-based fingerprinting. Noventra flag.",
     kOsAll, FEATURE_VALUE_TYPE(blink::features::kHeliumNoiseCanvas)},
    {helium::kHeliumNoiseAudioCommandLine,
     "[Noventra Noise] Jitter audio context data",
     "Adds insignificant jitter to audio data to deceive AudioContext-based "
     "fingerprinting. Noventra flag.",
     kOsAll, FEATURE_VALUE_TYPE(blink::features::kHeliumNoiseAudio)},
    {helium::kHeliumNoiseCpuCoresCommandLine,
     "[Noventra Noise] Randomize number of CPU cores",
     "Randomizes the number of cores returned by "
     "`navigator.hardwareConcurrency` in a reasonable range. Noventra flag.",
     kOsAll, FEATURE_VALUE_TYPE(blink::features::kHeliumNoiseCpuCores)},
    // core/network
    {helium::kDisableEchCommandLine, "Disable Encrypted Client Hello (ECH)",
     "Disables TLS Encrypted Client Hello, reducing traffic privacy when "
     "accessing websites that support ECH. Use this flag only if ECH is "
     "blocked on your network. Noventra flag.",
     kOsAll, SINGLE_VALUE_TYPE(helium::kDisableEchCommandLine)},
    {"quic-v2-only", "Use only QUIC v2 for HTTP/3",
     "Restricts HTTP/3 connections to QUIC v2 instead of default QUIC v1. This "
     "may help on networks that block QUIC v1 but do not recognize QUIC v2. It "
     "does not force QUIC on unsupported sites. Connections may fall back to "
     "TCP when the server does not support QUIC v2. Noventra flag.",
     kOsAll, SINGLE_VALUE_TYPE_AND_VALUE(switches::kQuicVersion, "RFCv2")},
    {helium::kHeliumSplitClientHelloCommandLine,
     "Random TLS ClientHello splitting",
     "Splits outgoing TLS ClientHello messages across two TLS records at a "
     "random point. The split targets the visible server name, or the ECH "
     "extension when ECH is used. This can bypass some domain-based network "
     "filtering, but it does not hide DNS queries or destination IP addresses "
     "and makes TLS traffic unusual and potentially recognizable. Some "
     "middleboxes and TLS endpoints mishandle this behavior and may become "
     "inaccessible. Noventra flag.",
     kOsAll, FEATURE_VALUE_TYPE(net::features::kHeliumSplitClientHello)},
    {"no-proxy-server", "Ignore system proxy",
     "Ignores system proxy settings and always connects directly. This may "
     "prevent access to networks that require a proxy. Noventra flag.",
     kOsAll, SINGLE_VALUE_TYPE(switches::kNoProxyServer)},
    // ui
    {helium::kHeliumNativeFrameMaterialsCommandLine, "Native frame materials",
     "Enables an option to apply native materials to the browser frame "
     "surfaces. May cause UI clarity issues. Noventra flag.",
     kOsMac, FEATURE_VALUE_TYPE(features::kHeliumNativeFrameMaterials)},
    {helium::kHeliumHidePageActionsCommandLine, "Hide all page actions",
     "Hides all page actions at the end of address bar. This includes: "
     "bookmark star, page zoom, PWA install/open, local file access, and more. "
     "Important actions and indicators will be missing. Noventra flag.",
     kOsDesktop, FEATURE_VALUE_TYPE(features::kHeliumHidePageActions)},
    // platform
#endif  // CHROME_BROWSER_HELIUM_FLAG_ENTRIES_H_
