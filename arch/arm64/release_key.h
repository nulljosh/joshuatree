/* CI's release key, public half (13.27.4): release.yml signs each release's kernel8.img with it (the JT_RELEASE_KEY secret), so a
   Pi updates straight from GitHub with no step on Joshua's Mac. It is trusted for the OS image only, never for an app
   (appsig.c's appsig_os_ok). Written by tools/release-key.sh; all zero means no CI key yet, and only the app key signs. */
static const unsigned char release_key[65] = { 0 };
