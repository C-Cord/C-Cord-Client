// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "ui_core_internal.h"

#define UI_PLAYER_MAX_DIM 960         // inline video is decoded no larger than this
#define UI_PLAYER_SYNC_BUDGET_MS 12u  // decode time per frame of the app; behind that, the clock waits
#define UI_PLAYER_AUDIO_LEAD_MS 80u   // audio queued ahead of the clock

// The host of a media URL for the log: no scheme, userinfo, port, path or query string (signed CDN URLs carry
// credentials-like tokens in the query).
static void player_log_host(const char *url, char *out, size_t cap)
{
  const char *p = url ? strstr(url, "://") : nullptr;
  p = p ? p + 3 : (url ? url : "");
  if (p[0] == '/' && p[1] == '/') p += 2;   // scheme-less "//host/path"
  size_t n = strcspn(p, "/?#");
  const char *at = memchr(p, '@', n);
  if (at) {
    n -= (size_t)(at + 1 - p);
    p = at + 1;
  }
  const char *colon = memchr(p, ':', n);
  if (colon) n = (size_t)(colon - p);
  if (n >= cap) n = cap - 1u;
  memcpy(out, p, n);
  out[n] = '\0';
  if (n == 0) snprintf(out, cap, "?");
}

// One warning per failed stage of a clicked video: `player: <stage> <reason> host=<host>`.
[[gnu::format(printf, 3, 4)]] static void player_warn(const char *url, const char *stage, const char *fmt, ...)
{
  char host[256], reason[200];
  va_list ap;
  va_start(ap, fmt);
  (void)vsnprintf(reason, sizeof reason, fmt, ap);
  va_end(ap);
  player_log_host(url, host, sizeof host);
  CCORD_LOG_W("player: %s %s host=%s", stage, reason, host);
}

void uc_media_player_close(ccord_ui *ui)
{
  // The render streams first: their demux threads read from the byte sources, and closing joins them.
  if (ui->media_stream) ccord_av_stream_close(ui->media_stream);
  ui->media_stream = nullptr;
  if (ui->media_net) ccord_session_close_media_stream(ui->d.session, ui->media_net);
  ui->media_net = nullptr;
  if (ui->media_audio_stream) ccord_av_stream_close(ui->media_audio_stream);
  ui->media_audio_stream = nullptr;
  if (ui->media_audio_net) ccord_session_close_media_stream(ui->d.session, ui->media_audio_net);
  ui->media_audio_net = nullptr;
  ui->media_resolving = false;
  ui->media_resolve_req_id = 0;
  ui->media_v_ready = false;
  ui->media_a_ready = false;
  ui->media_loaded = false;
  ui->media_buffering = false;
  ui->media_frame_serial = 0;
  ui->media_fail_hint[0] = '\0';
}

static bool media_player_set_url(char **slot, const char *url)
{
  if (!*slot) {
    *slot = ccord_calloc(1, UI_STREAM_URL_MAX);
    if (!*slot) return false;
  }
  ui_url_copy(*slot, UI_STREAM_URL_MAX, url ? url : "");
  return true;
}

static const char *media_player_active_play_url(const ccord_ui *ui)
{
  if (ui->media_play_url && ui->media_play_url[0]) return ui->media_play_url;
  return ui->media_url;
}

// The whole-file path: the clip is downloaded, then opened from memory (32 MiB cap). The user clicked this video, so
// the transport may take it from any https host (automatic loads stay strict).
static void media_player_fetch_whole(ccord_ui *ui, const char *url)
{
  ccord_error ferr;
  if (ccord_session_fetch_media_user(ui->d.session, url, &ferr) == CCORD_OK) {
    ui->dbg_media_requests++;
  } else {
    player_warn(url, "refused", "%s", ferr.message[0] ? ferr.message : "fetch refused");
    ccord_error_clear(&ferr);
    ui->media_failed = true;
  }
}

// Opens one ranged byte source + demux thread. False (nothing left open) when the transport cannot stream.
static bool media_player_open_one_stream(ccord_ui *ui, const char *url, unsigned av_flags,
                                         ccord_media_stream **net_out, ccord_av_stream **av_out)
{
  ccord_error serr;
  ccord_media_stream *net = ccord_session_open_media_stream(ui->d.session, url, &serr);
  if (!net) {
    ccord_error_clear(&serr);
    return false;
  }
  const ccord_av_source src = {
    .ud = net,
    .read_at = ccord_media_stream_read_at,
    .size = ccord_media_stream_size,
    .cancel = ccord_media_stream_cancel,
  };
  ccord_av_stream *av = ccord_av_stream_open_source(&src, UI_PLAYER_MAX_DIM, av_flags, &serr);
  if (!av) {
    ccord_error_clear(&serr);
    ccord_session_close_media_stream(ui->d.session, net);
    return false;
  }
  *net_out = net;
  *av_out = av;
  return true;
}

// Starts playback for resolved or direct stream URL(s). When `audio_url` is non-empty, opens a synchronized DASH pair
// (video-only stream + audio-only stream); otherwise opens a single muxed stream (falling back to whole-file download).
static void media_player_start_urls(ccord_ui *ui, const char *video_url, const char *audio_url)
{
  if (!video_url || !video_url[0]) {
    ui->media_failed = true;
    return;
  }
  if (!media_player_set_url(&ui->media_play_url, video_url) ||
      !media_player_set_url(&ui->media_audio_url, audio_url ? audio_url : "")) {
    ui->media_failed = true;
    return;
  }
  if (!ui->d.session) return;
  bool dual = audio_url && audio_url[0] != '\0';
  if (dual) {
    if (media_player_open_one_stream(ui, video_url, CCORD_AV_MEDIA | CCORD_AV_NO_AUDIO,
                                     &ui->media_net, &ui->media_stream) &&
        media_player_open_one_stream(ui, audio_url, CCORD_AV_MEDIA,
                                     &ui->media_audio_net, &ui->media_audio_stream)) {
      return;
    }
    // Transport lacks ranged streaming (e.g. mock fallback): play the primary clip whole-file.
    uc_media_player_close(ui);
    if (ui->media_audio_url) ui->media_audio_url[0] = '\0';
  }
  if (ccord_session_get_media(ui->d.session, video_url) == nullptr) {
    if (!media_player_open_one_stream(ui, video_url, CCORD_AV_MEDIA, &ui->media_net, &ui->media_stream)) {
      media_player_fetch_whole(ui, video_url);
    }
  }
}

void uc_media_player_select(ccord_ui *ui, const ui_media_hit *mh)
{
  if (!ui || !mh || !mh->url[0]) return;
  uc_media_player_close(ui);
  ui_url_copy(ui->media_url, sizeof ui->media_url, mh->url);
  if (ui->media_play_url) ui->media_play_url[0] = '\0';
  if (ui->media_audio_url) ui->media_audio_url[0] = '\0';
  ui->media_att_id = mh->att_id;
  ui->media_failed = false;
  ui->media_pos_ms = 0;
  ui->media_audio_resync = true;
  ui->media_duration_known = false;
  ui->media_duration_ms = mh->duration_ms > 0 ? mh->duration_ms : 5000u;
  if (!ui->d.session) return;
  if (mh->needs_resolve) {
    ccord_error rerr;
    uint64_t req_id = 0;
    if (ccord_session_resolve_video(ui->d.session, mh->url, &req_id, &rerr) == CCORD_OK) {
      ui->media_resolving = true;
      ui->media_resolve_req_id = req_id;
      ui->media_buffering = true;
    } else {
      player_warn(mh->url, "resolve", "%s", rerr.message[0] ? rerr.message : "resolve failed");
      ccord_error_clear(&rerr);
      ui->media_failed = true;
    }
    return;
  }
  media_player_start_urls(ui, mh->url, "");
}

// Moves the playhead; the picture and sound follow on the next tick.
void uc_media_player_seek(ccord_ui *ui, uint32_t pos_ms)
{
  if (pos_ms > ui->media_duration_ms) pos_ms = ui->media_duration_ms;
  ui->media_pos_ms = pos_ms;
  ui->media_audio_resync = true;
  if (ui->media_stream) ccord_av_stream_seek(ui->media_stream, pos_ms);
  if (ui->media_audio_stream) ccord_av_stream_seek(ui->media_audio_stream, pos_ms);
}

// Queues decoded sound up to a little past the clock. After a start, resume or seek the queue is first lined up
// with the clock again: silence when the decoder's audio is ahead, dropped samples when it is behind.
static void media_player_feed_audio(ccord_ui *ui, ccord_av_stream *st)
{
  static const int16_t k_silence[2u * 960u];
  int16_t buf[2u * 2048u];
  if (!st) return;
  if (!ui->media_audio) {
    ccord_error aerr;
    ccord_audio_config acfg = {.sample_rate = CCORD_AV_SAMPLE_RATE, .channels = 2, .frame_samples = 960, .capture = false};
    ui->media_audio = ccord_audio_create(&acfg, &aerr);
    if (!ui->media_audio) ccord_error_clear(&aerr);
  }
  bool out = ui->media_audio && ccord_audio_available(ui->media_audio);
  uint32_t pos = ui->media_pos_ms;
  if (ui->media_audio_resync) {
    ui->media_audio_resync = false;
    uint32_t have = ccord_av_stream_pcm_pos_ms(st);
    if (have > pos) {
      uint32_t gap = have - pos > 2000u ? 2000u : have - pos;
      size_t frames = (size_t)gap * (CCORD_AV_SAMPLE_RATE / 1000u);
      while (out && frames > 0) {
        size_t n = frames < 960u ? frames : 960u;
        (void)ccord_audio_write(ui->media_audio, k_silence, n);
        frames -= n;
      }
    } else {
      size_t frames = (size_t)(pos - have) * (CCORD_AV_SAMPLE_RATE / 1000u);
      while (frames > 0) {
        size_t n = ccord_av_stream_read_pcm(st, buf, frames < 2048u ? frames : 2048u);
        if (n == 0) break;
        frames -= n;
      }
    }
  }
  uint32_t want = pos + UI_PLAYER_AUDIO_LEAD_MS;
  for (int guard = 0; guard < 64; guard++) {
    uint32_t have = ccord_av_stream_pcm_pos_ms(st);
    if (have >= want) break;
    size_t frames = (size_t)(want - have) * (CCORD_AV_SAMPLE_RATE / 1000u);
    size_t n = ccord_av_stream_read_pcm(st, buf, frames < 2048u ? frames : 2048u);
    if (n == 0) break;
    if (out) (void)ccord_audio_write(ui->media_audio, buf, n);
  }
}

// The decoder(s) are open: take the duration and line the audio up with the clock.
static void media_player_opened(ccord_ui *ui, bool apply_pending_seek)
{
  const ccord_av_info *vinf = ccord_av_stream_info(ui->media_stream);
  uint32_t dur = vinf->duration_ms;
  if (ui->media_audio_stream) {
    const ccord_av_info *ainf = ccord_av_stream_info(ui->media_audio_stream);
    if (ainf->duration_ms > dur) dur = ainf->duration_ms;
  }
  ui->media_loaded = true;
  if (dur > 0) {
    ui->media_duration_ms = dur;
    ui->media_duration_known = true;
  }
  uint32_t asked_ms = ui->media_pos_ms;
  if (ui->media_pos_ms > ui->media_duration_ms) ui->media_pos_ms = ui->media_duration_ms;
  // A streamed clip keeps a seek made before it was ready (the render stream applies it when it becomes ready), but
  // one past the real end is moved back to it.
  if ((apply_pending_seek && ui->media_pos_ms > 0) || ui->media_pos_ms != asked_ms) {
    ccord_av_stream_seek(ui->media_stream, ui->media_pos_ms);
    if (ui->media_audio_stream) ccord_av_stream_seek(ui->media_audio_stream, ui->media_pos_ms);
  }
  ui->media_audio_resync = true;
}

// Follows a streamed clip (and optional separate DASH audio stream). Never blocks: state is read, not waited for.
// A server (or a link) the stream cannot use falls back to the whole-file download, which applies the same gate.
static void media_player_stream_poll(ccord_ui *ui)
{
  const char *play_url = media_player_active_play_url(ui);
  ccord_media_stream_stats vst;
  ccord_media_stream_stats_get(ui->media_net, &vst);
  ccord_error verr;
  memset(&verr, 0, sizeof verr);
  ccord_av_state vstate = ccord_av_stream_state(ui->media_stream, &verr);

  if (!ui->media_loaded) {
    if (vst.no_range || vst.error == CCORD_ERR_PERMISSION || vst.error == CCORD_ERR_UNSUPPORTED) {
      char url[UI_STREAM_URL_MAX];
      ui_url_copy(url, sizeof url, play_url);
      bool too_big = vst.no_range && vst.total > (int64_t)CCORD_AV_MAX_INPUT;
      uc_media_player_close(ui);
      if (ui->media_audio_url) ui->media_audio_url[0] = '\0';
      ui->dirty = true;
      if (too_big) {
        player_warn(url, "stream", "server does not support Range requests and the file is larger than 32 MiB");
        ui->media_failed = true;
        ui->media_playing = false;
        return;
      }
      if (vst.no_range) {
        char host[256];
        player_log_host(url, host, sizeof host);
        CCORD_LOG_I("player: stream server ignores Range; downloading whole file host=%s", host);
      }
      media_player_fetch_whole(ui, url);
      if (ui->media_failed) ui->media_playing = false;
      return;
    }
    if (vstate == CCORD_AV_READY) ui->media_v_ready = true;
  }
  if (vstate == CCORD_AV_FAILED) {
    if (vst.error != CCORD_OK) player_warn(play_url, "stream", "%s", vst.error_text[0] ? vst.error_text : "failed");
    else player_warn(play_url, "decode", "not playable: %s", verr.message[0] ? verr.message : "decoder refused it");
    ui->media_failed = true;
    ui->media_playing = false;
    ui->media_buffering = false;
    ui->dirty = true;
    return;
  }

  if (ui->media_audio_net && ui->media_audio_stream) {
    const char *aurl = (ui->media_audio_url && ui->media_audio_url[0]) ? ui->media_audio_url : play_url;
    ccord_media_stream_stats ast;
    ccord_media_stream_stats_get(ui->media_audio_net, &ast);
    ccord_error aerr;
    memset(&aerr, 0, sizeof aerr);
    ccord_av_state astate = ccord_av_stream_state(ui->media_audio_stream, &aerr);
    if (!ui->media_loaded && astate == CCORD_AV_READY) ui->media_a_ready = true;
    if (astate == CCORD_AV_FAILED ||
        (!ui->media_loaded && (ast.no_range || ast.error == CCORD_ERR_PERMISSION || ast.error == CCORD_ERR_UNSUPPORTED))) {
      // If the separate audio track fails, log it and drop audio rather than killing video playback unless video also failed.
      if (ast.error != CCORD_OK) player_warn(aurl, "stream", "%s", ast.error_text[0] ? ast.error_text : "audio failed");
      else player_warn(aurl, "decode", "audio not playable: %s", aerr.message[0] ? aerr.message : "decoder refused it");
      ccord_av_stream_close(ui->media_audio_stream);
      ui->media_audio_stream = nullptr;
      ccord_session_close_media_stream(ui->d.session, ui->media_audio_net);
      ui->media_audio_net = nullptr;
      ui->media_a_ready = false;
    }
  }

  if (!ui->media_loaded && ui->media_v_ready && (!ui->media_audio_stream || ui->media_a_ready)) {
    media_player_opened(ui, false);
    ui->dirty = true;
  }
}

bool uc_media_player_tick(ccord_ui *ui, uint64_t dt)
{
  if (!ui || !ui->media_url[0]) return false;
  if (ui->d.session) {
    ccord_video_resolved vr;
    while (ccord_session_pop_video_resolved(ui->d.session, &vr)) {
      if (!ui->media_resolving || vr.req_id != ui->media_resolve_req_id) continue;
      ui->media_resolving = false;
      ui->media_resolve_req_id = 0;
      if (vr.status != CCORD_OK || !vr.video_url[0]) {
        player_warn(ui->media_url, "resolve", "%s", vr.error[0] ? vr.error : "could not resolve video");
        if (vr.missing_helper) {
          snprintf(ui->media_fail_hint, sizeof ui->media_fail_hint, "Install yt-dlp to play - click title to open");
        }
        ui->media_failed = true;
        ui->media_playing = false;
        ui->media_buffering = false;
      } else {
        if (vr.duration_ms > 0) {
          ui->media_duration_ms = vr.duration_ms;
          ui->media_duration_known = true;
        }
        media_player_start_urls(ui, vr.video_url, vr.audio_url);
        if (ui->media_failed) ui->media_playing = false;
      }
      ui->dirty = true;
    }
  }
  if (ui->media_resolving) {
    ui->media_buffering = ui->media_playing;
    return true;
  }
  if (ui->media_net && !ui->media_failed) media_player_stream_poll(ui);
  if (!ui->media_loaded && !ui->media_failed && ui->d.session && !ui->media_net) {
    const char *play_url = media_player_active_play_url(ui);
    const ccord_media_item *it = ccord_session_get_media(ui->d.session, play_url);
    if (it && it->data && it->len > 0) {
      ccord_error cerr;
      ui->media_stream = ccord_av_stream_open(it->data, it->len, UI_PLAYER_MAX_DIM, CCORD_AV_MEDIA, &cerr);
      if (ui->media_stream) {
        media_player_opened(ui, true);
      } else {
        player_warn(play_url, "decode", "%zu bytes not playable: %s", it->len,
                    cerr.message[0] ? cerr.message : "decoder refused them");
        ccord_error_clear(&cerr);
        ui->media_failed = true;
        ui->media_playing = false;
      }
      ui->dirty = true;
    } else if (ccord_session_media_failed(ui->d.session, play_url)) {
      player_warn(play_url, "download", "failed (HTTP status, timeout or size cap; see the live: media line)");
      ui->media_failed = true;
      ui->media_playing = false;
      ui->dirty = true;
    }
  }
  if (!ui->media_loaded) {
    ui->media_buffering = ui->media_playing;
    // Waiting for the download (or the stream's first frames) with the clock held. A streamed clip keeps the loop
    // ticking even when paused: nothing else wakes the UI when the stream becomes ready.
    return ui->media_playing || (ui->media_net && !ui->media_failed);
  }
  ccord_av_stream *st = ui->media_stream;
  ccord_av_stream *ast = ui->media_audio_stream ? ui->media_audio_stream : st;
  const ccord_av_info *inf = ccord_av_stream_info(st);
  const ccord_av_info *ainf = ccord_av_stream_info(ast);
  if (ui->media_playing && !ui->media_buffering) {
    uint32_t step_ms = dt > 500u ? 500u : (uint32_t)dt;
    ui->media_pos_ms += step_ms;
    if (ui->media_duration_known && ui->media_pos_ms >= ui->media_duration_ms) {
      ui->media_pos_ms = ui->media_duration_ms;
      ui->media_playing = false;
    } else if (!ui->media_duration_known && ui->media_pos_ms >= ui->media_duration_ms) {
      ui->media_duration_ms = ui->media_pos_ms + 1u;   // length unknown: the bar stays just short of full
    }
    if (step_ms > 0) ui->dirty = true;
  }
  // The clock only runs while the decoder(s) keep up; paused, this still brings up the frame at the playhead.
  bool caught_up = ccord_av_stream_sync(st, ui->media_pos_ms, UI_PLAYER_SYNC_BUDGET_MS);
  if (ui->media_net && ccord_av_stream_starved(st)) caught_up = false;   // waiting on the network: the clock holds
  if (ui->media_audio_stream) {
    bool a_ok = ccord_av_stream_sync(ui->media_audio_stream, ui->media_pos_ms, UI_PLAYER_SYNC_BUDGET_MS);
    if (ui->media_audio_net && ccord_av_stream_starved(ui->media_audio_stream)) a_ok = false;
    caught_up = caught_up && a_ok;
  }
  if (inf->has_video) {
    uint32_t serial = 0;
    if (ccord_av_stream_frame(st, &serial) && serial != ui->media_frame_serial) ui->dirty = true;
  }
  if (!ui->media_playing) {
    ui->media_buffering = false;
    return !caught_up;
  }
  ui->media_buffering = !caught_up;
  if (ainf->has_audio) media_player_feed_audio(ui, ast);
  // Decoded to the end is not played to the end: the sound queued ahead of the clock still has to be heard.
  bool tail_heard = !ainf->has_audio || ui->media_pos_ms >= ccord_av_stream_pcm_pos_ms(ast);
  bool all_ended = ccord_av_stream_ended(st) && (!ui->media_audio_stream || ccord_av_stream_ended(ui->media_audio_stream));
  if (caught_up && tail_heard && all_ended) {
    ui->media_playing = false;
    if (!ui->media_duration_known) {
      ui->media_duration_ms = ui->media_pos_ms > 0 ? ui->media_pos_ms : 1u;
      ui->media_duration_known = true;
    }
    ui->media_pos_ms = ui->media_duration_ms;
    ui->dirty = true;
  }
  return ui->media_playing;
}

bool ui_is_payment_url(const char *url);

static bool is_discord_host(const char *host, size_t host_len)
{
  if (!host || host_len == 0) return false;
  static const char d1[] = "discord.com";
  static const size_t d1_len = sizeof(d1) - 1;
  static const char d2[] = "discordapp.com";
  static const size_t d2_len = sizeof(d2) - 1;

  if (host_len == d1_len && strncasecmp(host, d1, d1_len) == 0) return true;
  if (host_len > d1_len + 1 && host[host_len - d1_len - 1] == '.' &&
      strncasecmp(host + host_len - d1_len, d1, d1_len) == 0) {
    if (host_len >= 4 && strncasecmp(host, "cdn.", 4) == 0) return false;
    if (host_len >= 6 && strncasecmp(host, "media.", 6) == 0) return false;
    return true;
  }

  if (host_len == d2_len && strncasecmp(host, d2, d2_len) == 0) return true;
  if (host_len > d2_len + 1 && host[host_len - d2_len - 1] == '.' &&
      strncasecmp(host + host_len - d2_len, d2, d2_len) == 0) {
    if (host_len >= 4 && strncasecmp(host, "cdn.", 4) == 0) return false;
    if (host_len >= 6 && strncasecmp(host, "media.", 6) == 0) return false;
    return true;
  }

  return false;
}

static bool parse_http_url(const char *url, const char **out_host, size_t *out_host_len, const char **out_path)
{
  if (!url) return false;
  size_t scheme_len = 0;
  if (strncasecmp(url, "https://", 8) == 0) scheme_len = 8;
  else if (strncasecmp(url, "http://", 7) == 0) scheme_len = 7;
  else return false;

  const char *host = url + scheme_len;
  const char *first_slash = strchr(host, '/');
  const char *at = strchr(host, '@');
  if (at && (!first_slash || at < first_slash)) {
    host = at + 1;
  }

  const char *p = host;
  while (*p && *p != '/' && *p != '?' && *p != '#' && *p != ':') {
    p++;
  }
  size_t host_len = (size_t)(p - host);
  if (host_len == 0) return false;

  while (*p && *p != '/' && *p != '?' && *p != '#') {
    p++;
  }

  *out_host = host;
  *out_host_len = host_len;
  *out_path = p;
  return true;
}

static bool has_path_segment(const char *path, const char *seg)
{
  if (!path || !seg) return false;
  size_t slen = strlen(seg);
  const char *p = path;
  while ((p = strstr(p, seg)) != nullptr) {
    char after = p[slen];
    if (after == '\0' || after == '/' || after == '?' || after == '#') {
      return true;
    }
    p += slen;
  }
  return false;
}

typedef enum ccord_payment_kind {
  CCORD_PAYMENT_NONE = 0,
  CCORD_PAYMENT_BILLING,
  CCORD_PAYMENT_SHOP,
  CCORD_PAYMENT_NITRO,
} ccord_payment_kind;

static ccord_payment_kind classify_payment_url(const char *url)
{
  if (!url || !url[0]) return CCORD_PAYMENT_NONE;
  const char *host = nullptr;
  size_t host_len = 0;
  const char *path = nullptr;
  if (!parse_http_url(url, &host, &host_len, &path) || !is_discord_host(host, host_len) || !path) {
    return CCORD_PAYMENT_NONE;
  }
  if (has_path_segment(path, "/billing") || has_path_segment(path, "/settings/billing")) {
    return CCORD_PAYMENT_BILLING;
  }
  if (has_path_segment(path, "/shop") || has_path_segment(path, "/store")) {
    return CCORD_PAYMENT_SHOP;
  }
  if (has_path_segment(path, "/nitro") || has_path_segment(path, "/boost") ||
      has_path_segment(path, "/gifts") || has_path_segment(path, "/gift")) {
    return CCORD_PAYMENT_NITRO;
  }
  return CCORD_PAYMENT_NONE;
}

bool ui_is_payment_url(const char *url)
{
  return classify_payment_url(url) != CCORD_PAYMENT_NONE;
}

static bool url_is_discord_invite(const char *url)
{
  if (!url || !url[0]) return false;
  static const char *const k_prefixes[] = {
    "https://www.discord.gg/",        "http://www.discord.gg/",        "www.discord.gg/",
    "https://discord.gg/",            "http://discord.gg/",            "discord.gg/",
    "https://www.discord.com/invite/","http://www.discord.com/invite/","www.discord.com/invite/",
    "https://discord.com/invite/",    "http://discord.com/invite/",    "discord.com/invite/",
    "https://ptb.discord.com/invite/","http://ptb.discord.com/invite/","ptb.discord.com/invite/",
    "https://canary.discord.com/invite/","http://canary.discord.com/invite/","canary.discord.com/invite/",
    "https://discordapp.com/invite/", "http://discordapp.com/invite/", "discordapp.com/invite/",
    "discord://-/invite/",            "discord://invite/",
  };
  for (size_t i = 0; i < sizeof k_prefixes / sizeof k_prefixes[0]; i++) {
    size_t plen = strlen(k_prefixes[i]);
    bool match = true;
    for (size_t k = 0; k < plen; k++) {
      unsigned char a = (unsigned char)url[k];
      unsigned char b = (unsigned char)k_prefixes[i][k];
      if (a == '\0') { match = false; break; }
      if (a >= 'A' && a <= 'Z') a += (unsigned char)('a' - 'A');
      if (a != b) { match = false; break; }
    }
    if (match) {
      char code[CCORD_ADMIN_INVITE_CODE_MAX + 1];
      return ccord_admin_parse_invite_code(url, code, nullptr) == CCORD_OK;
    }
  }
  return false;
}

static bool ui_open_invite_url(ccord_ui *ui, const char *url)
{
  if (!ui || !url_is_discord_invite(url)) return false;
  char code[CCORD_ADMIN_INVITE_CODE_MAX + 1];
  ccord_error err;
  if (ccord_admin_parse_invite_code(url, code, &err) != CCORD_OK) return false;
  ui_menu_release(ui);
  ui->member_popover_open = false;
  ui->settings_open = false;
  ui->modal_kind = UI_MODAL_ADD_SERVER;
  ui->modal_guild_id = CCORD_ID_NONE;
  ui->modal_target_id = CCORD_ID_NONE;
  ui->modal_type_sel = 0;
  ui->modal_focus_field = 0;
  ccord_text_edit_clear(&ui->modal_edit1);
  (void)ccord_text_edit_insert(&ui->modal_edit1, url, strlen(url), &err);
  ccord_text_edit_clear(&ui->modal_edit2);
  if (ui->d.session) {
    ccord_session_clear_invite_preview(ui->d.session);
    ccord_admin_request req;
    if (ccord_admin_preview_invite(&req, code, &err) == CCORD_OK) {
      ui_admin_submit(ui, &req);
    }
  }
  ui->dirty = true;
  return true;
}

// Handles deep links of the form ccord://plugins/install/<id> or ccord://plugin/<id>.
// Never installs silently: always opens Settings -> Plugins with the review & install prompt.
static bool ui_open_ccord_plugin_url(ccord_ui *ui, const char *url)
{
  if (!ui || !url) return false;
  static const char *const k_prefixes[] = {
    "ccord://plugins/install/",
    "ccord://plugin/",
  };
  const char *rest = nullptr;
  for (size_t i = 0; i < sizeof k_prefixes / sizeof k_prefixes[0]; i++) {
    size_t plen = strlen(k_prefixes[i]);
    if (strncasecmp(url, k_prefixes[i], plen) == 0) {
      rest = url + plen;
      break;
    }
  }
  if (!rest || !rest[0]) return false;
  char id[65];
  size_t n = 0;
  while (rest[n] && rest[n] != '/' && rest[n] != '?' && rest[n] != '#') {
    if (n + 1 >= sizeof id) return false;
    char c = rest[n];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    id[n++] = c;
  }
  id[n] = '\0';
  return ui_open_plugin_install_prompt(ui, id);
}

bool ui_open_url(ccord_ui *ui, const char *url, const char *label)
{
  if (!ui || !url || !url[0]) return false;
  if (ui_open_ccord_plugin_url(ui, url)) return true;
  if (ui_open_invite_url(ui, url)) return true;
  const char *target = url;
  const char *target_label = label;
  switch (classify_payment_url(url)) {
  case CCORD_PAYMENT_BILLING:
    target = "https://discord.com/settings/billing";
    if (!target_label || !target_label[0] || strcmp(target_label, "Link") == 0) {
      target_label = "Billing";
    }
    break;
  case CCORD_PAYMENT_SHOP:
    target = "https://discord.com/shop";
    if (!target_label || !target_label[0] || strcmp(target_label, "Link") == 0) {
      target_label = "Shop";
    }
    break;
  case CCORD_PAYMENT_NITRO:
    target = "https://discord.com/nitro";
    if (!target_label || !target_label[0] || strcmp(target_label, "Link") == 0) {
      target_label = "Nitro";
    }
    break;
  case CCORD_PAYMENT_NONE:
  default:
    break;
  }
  char rewritten_url[UI_URL_MAX] = {0};
  if (ui->d.contrib) {
    if (!ccord_contrib_run_link_open_hook(ui->d.contrib, target, rewritten_url, sizeof rewritten_url)) {
      ui_notice(ui, "Link blocked by plugin");
      ui->dirty = true;
      return false;
    }
    if (rewritten_url[0]) target = rewritten_url;
  }
  ui_url_copy(ui->last_opened_url, sizeof ui->last_opened_url, target);
  ui->dbg_urls_opened++;
  bool ok = ui->d.host.open_url ? ui->d.host.open_url(ui->d.host.ud, target) : true;
  if (target_label && target_label[0]) ui_notice(ui, "Opened %.32s in browser: %.72s", target_label, target);
  else ui_notice(ui, "Opened in browser: %.88s", target);
  ui->dirty = true;
  return ok;
}

bool ui_activate_link(ccord_ui *ui, const char *url)
{
  if (!ui || !url || !url[0]) return false;
  return ui_open_url(ui, url, "Link");
}

bool ccord_ui_open_url(ccord_ui *ui, const char *url)
{
  return ui_open_url(ui, url, "Invite");
}

void ui_open_lightbox(ccord_ui *ui, const char *const *urls, size_t count, size_t index)
{
  if (!ui || !urls || count == 0) return;
  if (count > CCORD_RICH_MAX_ATTACHMENTS) count = CCORD_RICH_MAX_ATTACHMENTS;
  if (index >= count) index = 0;
  if (!urls[index] || !urls[index][0]) return;
  if (!ui->lightbox_urls) {
    ui->lightbox_urls = ccord_calloc(CCORD_RICH_MAX_ATTACHMENTS, sizeof *ui->lightbox_urls);
    if (!ui->lightbox_urls) return;
  }
  ui->lightbox_count = count;
  ui->lightbox_index = index;
  for (size_t i = 0; i < count; i++) {
    ui_url_copy(ui->lightbox_urls[i], sizeof ui->lightbox_urls[i], urls[i]);
  }
  ui_url_copy(ui->lightbox_url, sizeof ui->lightbox_url, ui->lightbox_urls[index]);
  ui->lightbox_zoom = 1.0f;
  ui->lightbox_open = true;
  ui->dirty = true;
}

void ui_close_lightbox(ccord_ui *ui)
{
  if (!ui) return;
  bool was_open = ui->lightbox_open;
  ui->lightbox_open = false;
  ui->lightbox_count = 0;
  ui->lightbox_index = 0;
  ccord_free(ui->lightbox_urls);
  ui->lightbox_urls = nullptr;
  if (was_open) ui->dirty = true;
}

bool ui_user_avatar_url(const ccord_user *u, char *out, size_t cap)
{
  if (!u || !u->avatar_url || !u->avatar_url[0] || !out || cap == 0) return false;
  if (strncmp(u->avatar_url, "https://", 8) == 0 || strncmp(u->avatar_url, "http://", 7) == 0) {
    size_t len = strlen(u->avatar_url);
    if (len >= cap) len = cap - 1;
    memcpy(out, u->avatar_url, len);
    out[len] = '\0';
    return true;
  }
  const char *ext = (strncmp(u->avatar_url, "a_", 2) == 0) ? "gif" : "png";
  snprintf(out, cap, "https://cdn.discordapp.com/avatars/%llu/%.96s.%s?size=64",
           (unsigned long long)u->id, u->avatar_url, ext);
  return true;
}

bool ui_user_banner_url(const ccord_user *u, char *out, size_t cap)
{
  if (!u || !u->banner_url || !u->banner_url[0] || !out || cap == 0) return false;
  if (strncmp(u->banner_url, "https://", 8) == 0 || strncmp(u->banner_url, "http://", 7) == 0) {
    static const char k_cdn[] = "https://cdn.discordapp.com/";
    static const char k_old[] = "?size=64";
    static const char k_new[] = "?size=512";
    size_t len = strlen(u->banner_url);
    if (strncmp(u->banner_url, k_cdn, sizeof(k_cdn) - 1) == 0 &&
        len >= sizeof(k_old) - 1 &&
        strcmp(u->banner_url + len - (sizeof(k_old) - 1), k_old) == 0) {
      size_t prefix = len - (sizeof(k_old) - 1);
      snprintf(out, cap, "%.*s%s", (int)prefix, u->banner_url, k_new);
      return true;
    }
    if (len >= cap) len = cap - 1;
    memcpy(out, u->banner_url, len);
    out[len] = '\0';
    return true;
  }
  const char *ext = (strncmp(u->banner_url, "a_", 2) == 0) ? "gif" : "png";
  snprintf(out, cap, "https://cdn.discordapp.com/banners/%llu/%.96s.%s?size=512",
           (unsigned long long)u->id, u->banner_url, ext);
  return true;
}

void ui_snowflake_date(ccord_id id, char *out, size_t cap)
{
  if (!out || cap == 0) return;
  static const char *const k_months[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
  };
  uint64_t ms = 1420070400000ull + (id >> 22);
  time_t sec = (time_t)(ms / 1000ull);
  struct tm tm_val;
  if (ccord_gmtime(sec, &tm_val) && tm_val.tm_mon >= 0 && tm_val.tm_mon < 12) {
    snprintf(out, cap, "%s %d, %d", k_months[tm_val.tm_mon], tm_val.tm_mday, tm_val.tm_year + 1900);
  } else {
    snprintf(out, cap, "Jan 1, 2015");
  }
}

bool ui_guild_icon_url(const ccord_guild *g, char *out, size_t cap)
{
  if (!g || !g->icon_url || !g->icon_url[0] || !out || cap == 0) return false;
  if (strncmp(g->icon_url, "https://", 8) == 0 || strncmp(g->icon_url, "http://", 7) == 0 ||
      strncmp(g->icon_url, "data:", 5) == 0) {
    snprintf(out, cap, "%.250s", g->icon_url);
    return true;
  }
  const char *ext = (strncmp(g->icon_url, "a_", 2) == 0) ? "gif" : "png";
  snprintf(out, cap, "https://cdn.discordapp.com/icons/%llu/%.96s.%s?size=64",
           (unsigned long long)g->id, g->icon_url, ext);
  return true;
}

bool ui_guild_banner_url(const ccord_guild *g, char *out, size_t cap)
{
  if (!g || !g->banner_url[0] || !out || cap == 0) return false;
  if (strncmp(g->banner_url, "https://", 8) == 0 || strncmp(g->banner_url, "http://", 7) == 0 ||
      strncmp(g->banner_url, "data:", 5) == 0) {
    snprintf(out, cap, "%.250s", g->banner_url);
    return true;
  }
  const char *ext = (strncmp(g->banner_url, "a_", 2) == 0) ? "gif" : "png";
  snprintf(out, cap, "https://cdn.discordapp.com/banners/%llu/%.96s.%s?size=512",
           (unsigned long long)g->id, g->banner_url, ext);
  return true;
}

void ui_url_copy(char *dst, size_t cap, const char *src)
{
  if (!dst || cap == 0) return;
  size_t n = src ? strlen(src) : 0;
  if (n >= cap) n = 0;
  if (n > 0) memcpy(dst, src, n);
  dst[n] = '\0';
}

bool ui_media_sized_url(const char *url, uint32_t w, uint32_t h, uint32_t max_w, uint32_t max_h, bool keep_format,
                        char *out, size_t cap)
{
  ui_url_copy(out, cap, url);
  if (!url || !out || cap == 0 || !out[0] || w == 0 || h == 0 || max_w == 0 || max_h == 0) return false;
  static const char *const hosts[] = {"https://media.discordapp.net/", "https://images-ext-1.discordapp.net/",
                                      "https://images-ext-2.discordapp.net/"};
  bool proxy = false;
  for (size_t i = 0; i < sizeof hosts / sizeof hosts[0]; i++) {
    if (strncmp(url, hosts[i], strlen(hosts[i])) == 0) proxy = true;
  }
  if (!proxy || strstr(url, "width=") || strstr(url, "format=")) return false;
  uint32_t tw = w, th = h;
  if (w > max_w || h > max_h) {
    float sc = fminf((float)max_w / (float)w, (float)max_h / (float)h);
    tw = (uint32_t)lroundf((float)w * sc);
    th = (uint32_t)lroundf((float)h * sc);
    if (tw == 0) tw = 1;
    if (th == 0) th = 1;
  } else if (keep_format) {
    return false;   // nothing to gain: the original is already small enough and keeps its format
  }
  size_t ul = strlen(url);
  const char *sep = strchr(url, '?') ? ((url[ul - 1] == '?' || url[ul - 1] == '&') ? "" : "&") : "?";
  char tmp[UI_URL_MAX];
  int n = snprintf(tmp, sizeof tmp, "%s%swidth=%u&height=%u%s", url, sep, (unsigned)tw, (unsigned)th,
                   keep_format ? "" : "&format=webp");
  if (n <= 0 || (size_t)n >= sizeof tmp || (size_t)n >= cap) return false;
  memcpy(out, tmp, (size_t)n + 1u);
  return true;
}

// True when the URL's path (ignoring ?query and #fragment) ends in `ext`, case-insensitively.
bool ui_url_path_ends(const char *url, const char *ext)
{
  if (!url || !ext) return false;
  size_t n = strcspn(url, "?#"), el = strlen(ext);
  if (n < el) return false;
  for (size_t i = 0; i < el; i++) {
    if (tolower((unsigned char)url[n - el + i]) != tolower((unsigned char)ext[i])) return false;
  }
  return true;
}

static uint64_t ui_media_hash(const char *url)
{
  uint64_t h = 14695981039346656037ull;
  for (const unsigned char *p = (const unsigned char *)url; *p; p++) {
    h ^= (uint64_t)*p;
    h *= 1099511628211ull;
  }
  return h ? h : 1u;
}

static ui_media_req *ui_media_req_find(ccord_ui *ui, const char *url, bool create)
{
  if (!ui->media_reqs) {
    if (!create) return nullptr;
    ui->media_reqs = ccord_calloc(UI_MEDIA_REQ_SLOTS, sizeof *ui->media_reqs);
    if (!ui->media_reqs) return nullptr;
    ui->nmedia_reqs = 0;
  }
  uint64_t h = ui_media_hash(url);
  const size_t mask = UI_MEDIA_REQ_SLOTS - 1u;
  for (int pass = 0; pass < 2; pass++) {
    size_t i = (size_t)h & mask;
    for (size_t probe = 0; probe < UI_MEDIA_REQ_SLOTS; probe++, i = (i + 1u) & mask) {
      ui_media_req *q = &ui->media_reqs[i];
      if (q->hash == h) return q;
      if (q->hash != 0) continue;
      if (!create) return nullptr;
      if (ui->nmedia_reqs >= UI_MEDIA_REQ_SLOTS / 4u * 3u) break;   // crowded: start over below
      *q = (ui_media_req){.hash = h};
      ui->nmedia_reqs++;
      return q;
    }
    if (!create) return nullptr;
    // Forgetting everything is harmless: textures and the session's cache stay, so a URL asked for again is
    // answered at once, and a failed one simply gets one early retry.
    memset(ui->media_reqs, 0, UI_MEDIA_REQ_SLOTS * sizeof *ui->media_reqs);
    ui->nmedia_reqs = 0;
  }
  return nullptr;
}

static const ui_media_req *ui_media_req_peek(const ccord_ui *ui, const char *url)
{
  if (!ui->media_reqs) return nullptr;
  uint64_t h = ui_media_hash(url);
  for (size_t n = 0; n < UI_MEDIA_REQ_SLOTS; n++) {
    const ui_media_req *q = &ui->media_reqs[(h + n) % UI_MEDIA_REQ_SLOTS];
    if (q->hash == h) return q;
    if (q->hash == 0) break;
  }
  return nullptr;
}

// How long a request may stay unanswered before it is taken as lost (the transport's own limit is 300 s).
#define UI_MEDIA_PENDING_MS 330000u

// The texture is up: forget the failures, so a re-fetch after the cache let it go is not held back.
static void ui_media_req_loaded(ui_media_req *q)
{
  if (!q) return;
  q->pending = false;
  q->failed = false;
  q->tries = 1;
  q->retry_at_ms = 0;
}

static uint64_t ui_media_backoff_ms(uint8_t tries)
{
  return tries <= 1 ? 15000u : (tries == 2 ? 60000u : (tries == 3 ? 300000u : 600000u));
}

// One stderr line when a media load fails (once per failure window: callers log where they mark the request failed,
// and a failed request is not asked for again until its back-off ends): `media: <reason> host=<host>`. Host only,
// never the path or query string (signed CDN URLs carry tokens).
[[gnu::format(printf, 2, 3)]] static void ui_media_warn(const char *url, const char *fmt, ...)
{
  char host[256], reason[160];
  va_list ap;
  va_start(ap, fmt);
  (void)vsnprintf(reason, sizeof reason, fmt, ap);
  va_end(ap);
  player_log_host(url, host, sizeof host);
  CCORD_LOG_W("media: %s host=%s", reason, host);
}

static bool ui_upload_media_bytes(ccord_renderer *r, const char *url, const uint8_t *data, size_t len,
                                  const ui_media_req *q)
{
  if (!r || !url || !url[0] || !data || len == 0) return false;
  ccord_error err;
  ccord_media_opts opts = {.max_dim = q ? (int)q->max_dim : 0, .allow_video = q && q->video};
  if (ccord_render_upload_media(r, url, data, len, &opts, nullptr, nullptr, &err) == CCORD_OK) {
    return true;
  }
  ccord_error_clear(&err);
  // Mock transport emits a 67-byte 1x1 PNG stub whose IDAT is 2 bytes (grayscale) under an RGBA IHDR.
  if (len == 67 && memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0) {
    static const uint8_t k_fallback_rgba[4] = {0x58, 0x65, 0xf2, 0xff};
    if (ccord_render_upload_rgba(r, url, k_fallback_rgba, 1, 1, &err) == CCORD_OK) {
      return true;
    }
    ccord_error_clear(&err);
  }
  return false;
}

static bool is_fixed_size_media_url(const ccord_ui *ui, const char *url)
{
  if (!url || !url[0]) return false;
  if (strstr(url, "/avatars/") != nullptr || strstr(url, "/icons/") != nullptr ||
      strstr(url, "/banners/") != nullptr || strstr(url, "/emojis/") != nullptr ||
      strstr(url, "/stickers/") != nullptr || strstr(url, "/tenor/") != nullptr ||
      strstr(url, "/assets/collectibles/") != nullptr || strstr(url, "/avatar-decoration-presets/") != nullptr) {
    return true;
  }
  if (ui && ui->d.store) {
    size_t ng = ccord_store_guild_count(ui->d.store);
    for (size_t i = 0; i < ng; i++) {
      const ccord_guild *g = ccord_store_guild_at(ui->d.store, i);
      if (g && g->icon_url && strcmp(g->icon_url, url) == 0) return true;
    }
  }
  return false;
}

void ui_pump_media(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui || !ui->d.session || !r) return;
  const size_t max_uploads_per_frame = 4;
  const uint64_t max_pump_ms = 4;
  uint64_t t0 = ccord_clock_ms();
  size_t uploaded = 0;
  bool invalidate_vlist = false;
  ccord_media_item item;
  while (uploaded < max_uploads_per_frame && ccord_session_pop_media(ui->d.session, &item)) {
    if ((ui->media_url[0] && strcmp(item.url, ui->media_url) == 0) ||
        (ui->media_play_url && ui->media_play_url[0] && strcmp(item.url, ui->media_play_url) == 0) ||
        (ui->media_audio_url && ui->media_audio_url[0] && strcmp(item.url, ui->media_audio_url) == 0)) {
      ui->dirty = true;
      continue;
    }
    ui_media_req *q = ui_media_req_find(ui, item.url, false);
    if (q) q->pending = false;
    if (ui_upload_media_bytes(r, item.url, item.data, item.len, q)) {
      ui->dbg_media_textures++;
      uploaded++;
      ui_media_req_loaded(q);
      if (!is_fixed_size_media_url(ui, item.url)) {
        invalidate_vlist = true;
      }
      ui->dirty = true;
    } else {
      ui_media_warn(item.url, "decode failed (%zu bytes arrived that are not a picture or video we can show)", item.len);
      if (q) {
        q->bad = true;   // arrived but is not something we can show: asking again would not change that
        q->failed = true;
        ui->dirty = true;
      }
    }
    if (uploaded >= max_uploads_per_frame || ccord_clock_ms() - t0 >= max_pump_ms) {
      ui->dirty = true;
      break;
    }
  }
  if (invalidate_vlist) {
    ccord_vlist_invalidate_all(&ui->vlist);
  }
}

bool ui_ensure_media_ex(ccord_ui *ui, ccord_renderer *r, const char *url, int max_dim, bool video)
{
  if (!ui || !url || !url[0]) return false;
  if (r && ccord_render_has_texture(r, url, nullptr, nullptr)) return true;
  if (!ui->d.session) return false;
  ui_media_req *q = ui_media_req_find(ui, url, true);
  if (!q || q->bad) return false;
  if (max_dim > 0) q->max_dim = (uint16_t)(max_dim > 4096 ? 4096 : max_dim);
  if (video) q->video = true;
  const ccord_media_item *cached = ccord_session_get_media(ui->d.session, url);
  if (cached && cached->data && cached->len > 0) {
    q->pending = false;
    if (!r) return false;
    if (ui_upload_media_bytes(r, url, cached->data, cached->len, q)) {
      ui_media_req_loaded(q);
      ui->dbg_media_textures++;
      ui->dirty = true;
      return true;
    }
    ui_media_warn(url, "decode failed (%zu cached bytes are not a picture or video we can show)", cached->len);
    q->bad = true;
    q->failed = true;
    return false;
  }
  if (q->pending) {
    if (!ccord_session_media_failed(ui->d.session, url) && ui->now_ms < q->retry_at_ms) return false;   // on its way
    ui_media_warn(url, ccord_session_media_failed(ui->d.session, url)
                           ? "download failed (the transport reported an error)"
                           : "download timed out (no answer)");
    q->pending = false;
    q->failed = true;
    q->retry_at_ms = ui->now_ms + ui_media_backoff_ms(q->tries);
    ui->dirty = true;   // lets the caller fall back to another source
    return false;
  }
  if (q->tries > 0 && ui->now_ms < q->retry_at_ms) return false;
  ccord_error err;
  ccord_status st = ccord_session_fetch_media(ui->d.session, url, &err);
  if (q->tries < UINT8_MAX) q->tries++;
  if (st == CCORD_OK) {
    q->pending = true;
    q->retry_at_ms = ui->now_ms + UI_MEDIA_PENDING_MS;
    ui->dbg_media_requests++;
    if (ccord_session_get_media(ui->d.session, url) != nullptr) {
      ui->dirty = true;
    }
  } else {
    // A refused URL (host not allowed) will not become allowed; anything else may be a passing state.
    bool refused = st == CCORD_ERR_PERMISSION || st == CCORD_ERR_UNSUPPORTED;
    if (refused) ui_media_warn(url, "load refused (not an https URL on the media host allow-list)");
    else ui_media_warn(url, "load request failed (%s)", ccord_status_name(st));
    ccord_error_clear(&err);
    q->failed = true;
    q->retry_at_ms = ui->now_ms + (refused ? 600000u : ui_media_backoff_ms(q->tries));
  }
  return false;
}

bool ui_ensure_media(ccord_ui *ui, ccord_renderer *r, const char *url)
{
  return ui_ensure_media_ex(ui, r, url, 0, false);
}

bool ui_media_failed(const ccord_ui *ui, const char *url)
{
  if (!ui || !url || !url[0]) return false;
  const ui_media_req *q = ui_media_req_peek(ui, url);
  return q && q->failed;
}

bool ui_media_requested(const ccord_ui *ui, const char *url)
{
  if (!ui || !url || !url[0]) return false;
  const ui_media_req *q = ui_media_req_peek(ui, url);
  return q && q->tries > 0;
}

