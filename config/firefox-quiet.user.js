/*
 * Optional dedicated-profile settings for the modem's very low-bandwidth
 * link. Copy this file to <Firefox profile>/user.js while Firefox is closed.
 * Firefox documents these automatic connections and preferences here:
 * https://support.mozilla.org/en-US/kb/how-stop-firefox-making-automatic-connections
 *
 * Use a dedicated profile: these settings remain active when the modem is not,
 * and deliberately suppress security-list and component updates on that
 * profile.  Keep a normally configured profile for ordinary browsing.
 */

/* Do not fetch/resolve/connect to links until the user actually requests one. */
user_pref("network.prefetch-next", false);
user_pref("network.dns.disablePrefetch", true);
user_pref("network.http.speculative-parallel-limit", 0);
user_pref("network.predictor.enabled", false);
user_pref("network.predictor.enable-prefetch", false);
user_pref("browser.urlbar.speculativeConnect.enabled", false);

/* Avoid periodic network-state probes and a second encrypted DNS path. */
user_pref("network.captive-portal-service.enabled", false);
user_pref("network.connectivity-service.enabled", false);
user_pref("network.trr.mode", 5);

/* Keep startup, new-tab, suggestions, recommendations, and Pocket quiet. */
user_pref("browser.startup.page", 0);
user_pref("browser.startup.homepage", "about:blank");
user_pref("browser.newtabpage.enabled", false);
user_pref("browser.search.suggest.enabled", false);
user_pref("browser.urlbar.suggest.searches", false);
user_pref("browser.urlbar.quicksuggest.enabled", false);
user_pref("browser.urlbar.quicksuggest.dataCollection.enabled", false);
user_pref("extensions.pocket.enabled", false);
user_pref("browser.region.update.enabled", false);
user_pref("browser.region.network.url", "");
user_pref("browser.search.geoip.url", "");
user_pref("browser.search.update", false);
user_pref("browser.startup.homepage_override.mstone", "ignore");
user_pref("browser.newtabpage.activity-stream.showSponsored", false);
user_pref("browser.newtabpage.activity-stream.showSponsoredTopSites", false);
user_pref("browser.newtabpage.activity-stream.asrouter.userprefs.cfr.addons", false);
user_pref("browser.newtabpage.activity-stream.asrouter.userprefs.cfr.features", false);
user_pref("browser.discovery.enabled", false);
user_pref("app.normandy.enabled", false);
user_pref("app.shield.optoutstudies.enabled", false);
user_pref("messaging-system.rsexperimentloader.enabled", false);
user_pref("extensions.getAddons.cache.enabled", false);

/* Avoid optional codecs, DRM components, and local ML model downloads. */
user_pref("media.gmp-gmpopenh264.enabled", false);
user_pref("media.gmp-widevinecdm.enabled", false);
user_pref("browser.ml.enable", false);

/* iMessage uses macOS APNs, not Firefox Web Push or Firefox Sync. */
user_pref("dom.push.connection.enabled", false);
user_pref("identity.fxaccounts.enabled", false);

/* Disable optional diagnostics for this dedicated low-bandwidth profile. */
user_pref("datareporting.healthreport.uploadEnabled", false);
user_pref("datareporting.usage.uploadEnabled", false);
user_pref("toolkit.telemetry.enabled", false);
user_pref("toolkit.telemetry.unified", false);
user_pref("browser.ping-centre.telemetry", false);
user_pref("browser.newtabpage.activity-stream.telemetry", false);
