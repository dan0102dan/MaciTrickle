/* Platform filesystem paths — C equivalent of Go's build-tag-conditional
 * constant/path_{default,entware,openwrt}.go. Defaults here match
 * path_default.go; a real per-platform build overrides each macro via
 * -D (same override hook already used for MT_CONFIG_PATH in main.c).
 * Wiring these into the Makefile's per-PLATFORM/TARGET build is Phase 8
 * packaging work; until then every C build uses the defaults below.
 */
#ifndef MAGITRICKLE_PATHS_H
#define MAGITRICKLE_PATHS_H

#ifndef MT_APP_SHARE_DIR
#define MT_APP_SHARE_DIR "/usr/share/magitrickle"
#endif
#ifndef MT_APP_STATE_DIR
#define MT_APP_STATE_DIR "/var/lib/magitrickle"
#endif
#ifndef MT_SOCK_PATH
#define MT_SOCK_PATH "/var/run/magitrickle.sock"
#endif
#ifndef MT_PASSWD_FILE
#define MT_PASSWD_FILE "/etc/passwd"
#endif
#ifndef MT_SHADOW_FILE
#define MT_SHADOW_FILE "/etc/shadow"
#endif

#endif /* MAGITRICKLE_PATHS_H */
