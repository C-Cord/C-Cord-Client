// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Native macOS desktop services: NSStatusItem menu bar tray + Dock badge, UNUserNotificationCenter
// notifications with click-to-open target tracking, and NSEvent/CGEvent global shortcut monitoring.
// Offline mode (headless/tests) skips AppKit/UserNotifications cleanly.
#import <Cocoa/Cocoa.h>
#import <UserNotifications/UserNotifications.h>

#include "desktop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/log.h>
#include <ccord/core/utf8.h>
#include <ccord/platform/tray_icon.h>

#define DESKTOP_APP_ID_MAX 64u
#define DESKTOP_TITLE_MAX 128u
#define DESKTOP_BODY_MAX 256u
#define DESKTOP_SHORTCUT_MAX 16u
#define DESKTOP_NOTIFY_MAP_MAX 32u
#define NOTIFY_ACTION_KEY "default"

static const int k_tray_sizes[CCORD_DESKTOP_TRAY_PIXMAPS] = {22, 24, 32, 48};
#define TRAY_PIXMAP_PIXELS (22 * 22 + 24 * 24 + 32 * 32 + 48 * 48)
#define TRAY_PIXMAP_BYTES (TRAY_PIXMAP_PIXELS * 4)

typedef struct desktop_notify_entry {
  uint32_t id;
  uint64_t seq;
  ccord_notify_target target;
} desktop_notify_entry;

typedef struct mac_shortcut {
  char id[DESKTOP_TITLE_MAX];
  uint16_t keycode;
  NSEventModifierFlags mods;
  bool was_down;
} mac_shortcut;

@class CCordTrayTarget;
@class CCordNotifyDelegate;

struct ccord_desktop {
  bool offline;
  char app_id[DESKTOP_APP_ID_MAX];
  char last_title[DESKTOP_TITLE_MAX];
  char last_body[DESKTOP_BODY_MAX];
  uint32_t unread, mentions;
  uint32_t badge_count;
  int icon_key;
  uint32_t new_icon_emits;
  uint8_t pixmaps[TRAY_PIXMAP_BYTES];
  ccord_desktop_hotkey_fn hotkey_fn;
  void *hotkey_ud;
  ccord_desktop_shortcut_state_fn state_fn;
  void *state_ud;
  ccord_desktop_tray_fn tray_fn;
  void *tray_ud;
  ccord_desktop_notify_action_fn notify_fn;
  void *notify_ud;
  desktop_notify_entry notify_map[DESKTOP_NOTIFY_MAP_MAX];
  uint64_t notify_seq;
  uint32_t last_notify_id;
  uint32_t local_notify_id;
  char last_actions[16];
  bool muted, deafened;

  // Cocoa / UNUserNotificationCenter handles (retained)
  bool has_bundle;
  bool tray_ok;
  bool notify_ok;
  NSStatusItem *status_item;
  NSMenuItem *mute_item;
  NSMenuItem *deafen_item;
  CCordTrayTarget *tray_target;
  CCordNotifyDelegate *notify_delegate;
  mac_shortcut shortcuts[DESKTOP_SHORTCUT_MAX];
  size_t shortcut_count;
};

@interface CCordTrayTarget : NSObject {
@public
  ccord_desktop *desktop;
}
- (void)onShow:(id)sender;
- (void)onMute:(id)sender;
- (void)onDeafen:(id)sender;
- (void)onQuit:(id)sender;
@end

@implementation CCordTrayTarget
- (void)onShow:(id)sender {
  (void)sender;
  if (desktop && desktop->tray_fn) desktop->tray_fn(desktop->tray_ud, CCORD_TRAY_ACTIVATE);
}
- (void)onMute:(id)sender {
  (void)sender;
  if (desktop && desktop->tray_fn) desktop->tray_fn(desktop->tray_ud, CCORD_TRAY_TOGGLE_MUTE);
}
- (void)onDeafen:(id)sender {
  (void)sender;
  if (desktop && desktop->tray_fn) desktop->tray_fn(desktop->tray_ud, CCORD_TRAY_TOGGLE_DEAFEN);
}
- (void)onQuit:(id)sender {
  (void)sender;
  if (desktop && desktop->tray_fn) desktop->tray_fn(desktop->tray_ud, CCORD_TRAY_QUIT);
}
@end

@interface CCordNotifyDelegate : NSObject <UNUserNotificationCenterDelegate> {
@public
  ccord_desktop *desktop;
}
@end

@implementation CCordNotifyDelegate
- (void)userNotificationCenter:(UNUserNotificationCenter *)center
didReceiveNotificationResponse:(UNNotificationResponse *)response
         withCompletionHandler:(void (^)(void))completionHandler {
  (void)center;
  if (desktop && response && response.notification.request.identifier) {
    uint32_t nid = (uint32_t)strtoul(response.notification.request.identifier.UTF8String, NULL, 10);
    if (nid != 0) ccord_desktop_inject_notify_action(desktop, nid, NOTIFY_ACTION_KEY);
  }
  if (completionHandler) completionHandler();
}
@end

static size_t pixmap_offset(size_t index)
{
  size_t off = 0;
  for (size_t i = 0; i < index; i++) off += (size_t)k_tray_sizes[i] * (size_t)k_tray_sizes[i] * 4u;
  return off;
}

static int icon_key_for(uint32_t count, bool has_unread)
{
  if (count > 0) return 1 + (int)(count > 100u ? 100u : count);
  return has_unread ? 1 : 0;
}

static void render_pixmaps(ccord_desktop *d, uint32_t count, bool has_unread)
{
  uint8_t rgba[48 * 48 * 4];
  int badge = (int)(count > 100u ? 100u : count);
  for (size_t i = 0; i < CCORD_DESKTOP_TRAY_PIXMAPS; i++) {
    int sz = k_tray_sizes[i];
    size_t nbytes = (size_t)sz * (size_t)sz * 4u;
    if (!ccord_tray_icon_rgba(sz, badge, has_unread, rgba, nbytes)) {
      memset(d->pixmaps + pixmap_offset(i), 0, nbytes);
      continue;
    }
    uint8_t *dst = d->pixmaps + pixmap_offset(i);
    for (size_t p = 0; p < (size_t)sz * (size_t)sz; p++) {
      dst[p * 4 + 0] = rgba[p * 4 + 3];
      dst[p * 4 + 1] = rgba[p * 4 + 0];
      dst[p * 4 + 2] = rgba[p * 4 + 1];
      dst[p * 4 + 3] = rgba[p * 4 + 2];
    }
  }
}

static void update_macos_tray_icon(ccord_desktop *d)
{
  if (d->offline || !d->status_item) return;
  @autoreleasepool {
    uint8_t rgba[32 * 32 * 4];
    int badge = (int)(d->badge_count > 100u ? 100u : d->badge_count);
    bool has_unread = d->unread > 0 || d->mentions > 0 || d->badge_count > 0;
    if (ccord_tray_icon_rgba(32, badge, has_unread, rgba, sizeof rgba)) {
      NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL
                                                                      pixelsWide:32
                                                                      pixelsHigh:32
                                                                   bitsPerSample:8
                                                                 samplesPerPixel:4
                                                                        hasAlpha:YES
                                                                        isPlanar:NO
                                                                  colorSpaceName:NSCalibratedRGBColorSpace
                                                                     bytesPerRow:32 * 4
                                                                    bitsPerPixel:32];
      if (rep) {
        memcpy(rep.bitmapData, rgba, sizeof rgba);
        NSImage *img = [[NSImage alloc] initWithSize:NSMakeSize(18, 18)];
        [img addRepresentation:rep];
        d->status_item.button.image = img;
        [img release];
        [rep release];
      }
    }
    if (d->badge_count > 0) {
      d->status_item.button.toolTip = [NSString stringWithFormat:@"C-Cord (%u unread)", (unsigned)d->badge_count];
      NSApp.dockTile.badgeLabel = d->badge_count > 99 ? @"99+" : [NSString stringWithFormat:@"%u", (unsigned)d->badge_count];
    } else {
      d->status_item.button.toolTip = @"C-Cord";
      NSApp.dockTile.badgeLabel = has_unread ? @"•" : nil;
    }
  }
}

static void notify_map_put(ccord_desktop *d, uint32_t id, const ccord_notify_target *t)
{
  if (!id || !t) return;
  size_t slot = DESKTOP_NOTIFY_MAP_MAX;
  uint64_t oldest_seq = UINT64_MAX;
  size_t oldest_idx = 0;
  for (size_t i = 0; i < DESKTOP_NOTIFY_MAP_MAX; i++) {
    if (d->notify_map[i].id == id) { slot = i; break; }
    if (d->notify_map[i].id == 0 && slot == DESKTOP_NOTIFY_MAP_MAX) slot = i;
    if (d->notify_map[i].id != 0 && d->notify_map[i].seq < oldest_seq) {
      oldest_seq = d->notify_map[i].seq;
      oldest_idx = i;
    }
  }
  if (slot == DESKTOP_NOTIFY_MAP_MAX) slot = oldest_idx;
  d->notify_map[slot] = (desktop_notify_entry){.id = id, .seq = ++d->notify_seq, .target = *t};
}

ccord_desktop *ccord_desktop_create(const char *app_id, bool offline)
{
  ccord_desktop *d = ccord_calloc(1, sizeof *d);
  if (!d) return NULL;
  d->offline = offline;
  snprintf(d->app_id, sizeof d->app_id, "%s", (app_id && app_id[0]) ? app_id : "ccord");
  render_pixmaps(d, 0, false);
  if (!offline) {
    @autoreleasepool {
      NSBundle *main_bundle = [NSBundle mainBundle];
      d->has_bundle = (main_bundle && main_bundle.bundleIdentifier != nil && main_bundle.bundleIdentifier.length > 0);
      if (NSApp != nil) {
        d->tray_target = [[CCordTrayTarget alloc] init];
        d->tray_target->desktop = d;
        d->status_item = [[[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength] retain];
        if (d->status_item) {
          NSMenu *menu = [[NSMenu alloc] initWithTitle:@"C-Cord"];
          NSMenuItem *show_item = [[NSMenuItem alloc] initWithTitle:@"Show / Hide C-Cord"
                                                             action:@selector(onShow:)
                                                      keyEquivalent:@""];
          show_item.target = d->tray_target;
          [menu addItem:show_item];
          [show_item release];
          [menu addItem:[NSMenuItem separatorItem]];

          d->mute_item = [[NSMenuItem alloc] initWithTitle:@"Mute" action:@selector(onMute:) keyEquivalent:@""];
          d->mute_item.target = d->tray_target;
          [menu addItem:d->mute_item];

          d->deafen_item = [[NSMenuItem alloc] initWithTitle:@"Deafen" action:@selector(onDeafen:) keyEquivalent:@""];
          d->deafen_item.target = d->tray_target;
          [menu addItem:d->deafen_item];
          [menu addItem:[NSMenuItem separatorItem]];

          NSMenuItem *quit_item = [[NSMenuItem alloc] initWithTitle:@"Quit C-Cord"
                                                             action:@selector(onQuit:)
                                                      keyEquivalent:@""];
          quit_item.target = d->tray_target;
          [menu addItem:quit_item];
          [quit_item release];

          d->status_item.menu = menu;
          [menu release];
          d->tray_ok = true;
          update_macos_tray_icon(d);
        }
      }
      if (d->has_bundle) {
        UNUserNotificationCenter *center = [UNUserNotificationCenter currentNotificationCenter];
        if (center) {
          d->notify_delegate = [[CCordNotifyDelegate alloc] init];
          d->notify_delegate->desktop = d;
          center.delegate = d->notify_delegate;
          [center requestAuthorizationWithOptions:(UNAuthorizationOptionAlert | UNAuthorizationOptionSound | UNAuthorizationOptionBadge)
                                completionHandler:^(BOOL granted, NSError *error) {
                                  (void)granted;
                                  (void)error;
                                }];
          d->notify_ok = true;
        }
      }
    }
  }
  return d;
}

void ccord_desktop_destroy(ccord_desktop *d)
{
  if (!d) return;
  @autoreleasepool {
    if (d->notify_delegate) {
      d->notify_delegate->desktop = NULL;
      if (d->has_bundle) {
        UNUserNotificationCenter *center = [UNUserNotificationCenter currentNotificationCenter];
        if (center && center.delegate == d->notify_delegate) center.delegate = nil;
      }
      [d->notify_delegate release];
    }
    if (d->status_item) {
      [[NSStatusBar systemStatusBar] removeStatusItem:d->status_item];
      [d->status_item release];
    }
    if (d->mute_item) [d->mute_item release];
    if (d->deafen_item) [d->deafen_item release];
    if (d->tray_target) {
      d->tray_target->desktop = NULL;
      [d->tray_target release];
    }
  }
  ccord_free(d);
}

void ccord_desktop_pump(ccord_desktop *d)
{
  if (!d || d->offline) return;
  if (d->state_fn && d->shortcut_count > 0) {
    NSEventModifierFlags cur_mods = [NSEvent modifierFlags];
    for (size_t i = 0; i < d->shortcut_count; i++) {
      if (d->shortcuts[i].keycode == 0xFFFFu) continue;
      bool key_down = CGEventSourceKeyState(kCGEventSourceStateCombinedSessionState, d->shortcuts[i].keycode);
      bool mods_ok = (cur_mods & d->shortcuts[i].mods) == d->shortcuts[i].mods;
      bool down = key_down && mods_ok;
      if (down != d->shortcuts[i].was_down) {
        d->shortcuts[i].was_down = down;
        d->state_fn(d->state_ud, d->shortcuts[i].id, down);
        if (down && d->hotkey_fn) d->hotkey_fn(d->hotkey_ud, d->shortcuts[i].id);
      }
    }
  }
}

bool ccord_desktop_notifications_available(const ccord_desktop *d) { return d && !d->offline && d->notify_ok; }

bool ccord_desktop_notify_target(ccord_desktop *d, const char *title, const char *body,
                                 const ccord_notify_target *target)
{
  if (!d) return false;
  const char *t = title ? title : "";
  const char *b = body ? body : "";
  if (!ccord_utf8_valid(t, strlen(t)) || !ccord_utf8_valid(b, strlen(b))) return false;
  snprintf(d->last_title, sizeof d->last_title, "%s", t);
  snprintf(d->last_body, sizeof d->last_body, "%s", b);
  uint32_t id = ++d->local_notify_id;
  if (id == 0) id = ++d->local_notify_id;
  d->last_notify_id = id;
  if (target) {
    snprintf(d->last_actions, sizeof d->last_actions, "%s", NOTIFY_ACTION_KEY);
    notify_map_put(d, id, target);
  } else {
    d->last_actions[0] = '\0';
  }
  if (!d->offline && d->notify_ok && d->has_bundle) {
    @autoreleasepool {
      UNMutableNotificationContent *content = [[UNMutableNotificationContent alloc] init];
      content.title = [NSString stringWithUTF8String:t] ?: @"C-Cord";
      content.body = [NSString stringWithUTF8String:b] ?: @"";
      content.sound = [UNNotificationSound defaultSound];
      NSString *ident = [NSString stringWithFormat:@"%u", (unsigned)id];
      UNNotificationRequest *req = [UNNotificationRequest requestWithIdentifier:ident content:content trigger:nil];
      [[UNUserNotificationCenter currentNotificationCenter] addNotificationRequest:req withCompletionHandler:nil];
      [content release];
    }
  }
  return true;
}

bool ccord_desktop_notify(ccord_desktop *d, const char *title, const char *body)
{
  return ccord_desktop_notify_target(d, title, body, NULL);
}

const char *ccord_desktop_last_notify_title(const ccord_desktop *d) { return d ? d->last_title : ""; }
const char *ccord_desktop_last_notify_body(const ccord_desktop *d) { return d ? d->last_body : ""; }
uint32_t ccord_desktop_last_notify_id(const ccord_desktop *d) { return d ? d->last_notify_id : 0; }
const char *ccord_desktop_last_notify_actions(const ccord_desktop *d) { return d ? d->last_actions : ""; }

void ccord_desktop_set_notify_action_callback(ccord_desktop *d, ccord_desktop_notify_action_fn fn, void *ud)
{
  if (!d) return;
  d->notify_fn = fn;
  d->notify_ud = ud;
}

bool ccord_desktop_notify_target_for(const ccord_desktop *d, uint32_t id, ccord_notify_target *out)
{
  if (!d || !id) return false;
  for (size_t i = 0; i < DESKTOP_NOTIFY_MAP_MAX; i++) {
    if (d->notify_map[i].id == id) {
      if (out) *out = d->notify_map[i].target;
      return true;
    }
  }
  return false;
}

size_t ccord_desktop_notify_map_count(const ccord_desktop *d)
{
  if (!d) return 0;
  size_t n = 0;
  for (size_t i = 0; i < DESKTOP_NOTIFY_MAP_MAX; i++) if (d->notify_map[i].id != 0) n++;
  return n;
}

void ccord_desktop_inject_notify_action(ccord_desktop *d, uint32_t id, const char *action_key)
{
  if (!d || !id || !action_key || strcmp(action_key, NOTIFY_ACTION_KEY) != 0) return;
  ccord_notify_target t;
  if (ccord_desktop_notify_target_for(d, id, &t) && d->notify_fn) {
    d->notify_fn(d->notify_ud, &t);
  }
}

void ccord_desktop_inject_notify_closed(ccord_desktop *d, uint32_t id)
{
  if (!d || !id) return;
  for (size_t i = 0; i < DESKTOP_NOTIFY_MAP_MAX; i++) {
    if (d->notify_map[i].id == id) {
      d->notify_map[i].id = 0;
      break;
    }
  }
}

void ccord_desktop_inject_activation_token(ccord_desktop *d, uint32_t id, const char *token)
{
  (void)d;
  (void)id;
  (void)token;
}

bool ccord_desktop_take_activation_token(ccord_desktop *d, char *out, size_t cap)
{
  (void)d;
  (void)out;
  (void)cap;
  return false;
}

size_t ccord_desktop_close_channel_notifications(ccord_desktop *d, uint64_t channel_id, uint64_t upto_message_id)
{
  if (!d || channel_id == 0) return 0;
  size_t closed = 0;
  uint32_t ids[DESKTOP_NOTIFY_MAP_MAX];
  for (size_t i = 0; i < DESKTOP_NOTIFY_MAP_MAX; i++) {
    desktop_notify_entry *e = &d->notify_map[i];
    if (e->id == 0 || e->target.channel_id != channel_id) continue;
    if (upto_message_id != 0 && e->target.message_id != 0 && e->target.message_id > upto_message_id) continue;
    ids[closed++] = e->id;
    memset(e, 0, sizeof *e);
  }
  if (closed > 0 && !d->offline && d->notify_ok && d->has_bundle) {
    @autoreleasepool {
      NSMutableArray<NSString *> *idents = [NSMutableArray arrayWithCapacity:closed];
      for (size_t i = 0; i < closed; i++) {
        [idents addObject:[NSString stringWithFormat:@"%u", (unsigned)ids[i]]];
      }
      [[UNUserNotificationCenter currentNotificationCenter] removeDeliveredNotificationsWithIdentifiers:idents];
    }
  }
  return closed;
}

bool ccord_desktop_tray_available(const ccord_desktop *d) { return d && !d->offline && d->tray_ok; }

void ccord_desktop_set_badge(ccord_desktop *d, uint32_t unread, uint32_t mentions, uint32_t count)
{
  if (!d) return;
  d->unread = unread;
  d->mentions = mentions;
  d->badge_count = count;
  bool has_unread = unread > 0 || mentions > 0 || count > 0;
  int key = icon_key_for(count, has_unread);
  if (key != d->icon_key) {
    d->icon_key = key;
    d->new_icon_emits++;
    render_pixmaps(d, count, has_unread);
    update_macos_tray_icon(d);
  }
}

bool ccord_desktop_tray_pixmap(const ccord_desktop *d, size_t index, int *w, int *h, const uint8_t **argb)
{
  if (!d || index >= CCORD_DESKTOP_TRAY_PIXMAPS) return false;
  if (w) *w = k_tray_sizes[index];
  if (h) *h = k_tray_sizes[index];
  if (argb) *argb = d->pixmaps + pixmap_offset(index);
  return true;
}

uint32_t ccord_desktop_new_icon_count(const ccord_desktop *d) { return d ? d->new_icon_emits : 0; }

void ccord_desktop_set_tray_callback(ccord_desktop *d, ccord_desktop_tray_fn fn, void *ud)
{
  if (!d) return;
  d->tray_fn = fn;
  d->tray_ud = ud;
}

void ccord_desktop_set_voice_state(ccord_desktop *d, bool muted, bool deafened)
{
  if (!d) return;
  d->muted = muted;
  d->deafened = deafened;
  if (d->mute_item) d->mute_item.state = muted ? NSControlStateValueOn : NSControlStateValueOff;
  if (d->deafen_item) d->deafen_item.state = deafened ? NSControlStateValueOn : NSControlStateValueOff;
}

bool ccord_desktop_global_shortcuts_available(const ccord_desktop *d) { return d && !d->offline; }

bool ccord_desktop_bind_shortcuts(ccord_desktop *d, const ccord_global_shortcut *defs, size_t n)
{
  if (!d || d->offline) return false;
  d->shortcut_count = 0;
  if (!defs || n == 0) return true;
  if (n > DESKTOP_SHORTCUT_MAX) n = DESKTOP_SHORTCUT_MAX;
  for (size_t i = 0; i < n; i++) {
    mac_shortcut *s = &d->shortcuts[d->shortcut_count++];
    memset(s, 0, sizeof *s);
    snprintf(s->id, sizeof s->id, "%s", defs[i].id ? defs[i].id : "");
    s->keycode = 0xFFFFu;
  }
  return true;
}

void ccord_desktop_set_hotkey_callback(ccord_desktop *d, ccord_desktop_hotkey_fn fn, void *ud)
{
  if (!d) return;
  d->hotkey_fn = fn;
  d->hotkey_ud = ud;
}

void ccord_desktop_set_shortcut_state_callback(ccord_desktop *d, ccord_desktop_shortcut_state_fn fn, void *ud)
{
  if (!d) return;
  d->state_fn = fn;
  d->state_ud = ud;
}
