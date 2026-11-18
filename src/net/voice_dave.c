// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/net/voice_dave.h>

#include <stdio.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/log.h>

#ifdef CCORD_HAVE_DAVE
#include <dave/dave.h>
#endif

#define DAVE_INIT_TRANSITION_ID 0u
#define DAVE_NEW_GROUP_EPOCH 1u
#define DAVE_MAX_TRANSITIONS 16u

ccord_status ccord_vframe_parse(const uint8_t *buf, size_t len, ccord_vframe *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!buf || !out || len < 3) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "voice binary frame requires >= 3 bytes");
  }
  out->seq = (uint16_t)(((uint16_t)buf[0] << 8) | buf[1]);
  out->op = buf[2];
  out->payload = buf + 3;
  out->payload_len = len - 3;
  return CCORD_OK;
}

size_t ccord_vframe_build(uint8_t op, const uint8_t *payload, size_t payload_len, uint8_t *out, size_t cap)
{
  if (!out || cap < 1 || payload_len > cap - 1 || (payload_len > 0 && !payload)) return 0;
  out[0] = op;
  if (payload_len > 0) memcpy(out + 1, payload, payload_len);
  return 1 + payload_len;
}

ccord_status ccord_vframe_split_transition(const uint8_t *payload,
                                           size_t len,
                                           uint16_t *transition_id,
                                           const uint8_t **mls,
                                           size_t *mls_len,
                                           ccord_error *err)
{
  ccord_error_clear(err);
  if (!payload || len < 2) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "DAVE transition payload requires >= 2 bytes");
  }
  if (transition_id) *transition_id = (uint16_t)(((uint16_t)payload[0] << 8) | payload[1]);
  if (mls) *mls = payload + 2;
  if (mls_len) *mls_len = len - 2;
  return CCORD_OK;
}

typedef struct dave_transition {
  uint16_t id;
  uint16_t version;
  bool used;
} dave_transition;

typedef struct dave_peer {
  ccord_id user_id;
  char user_str[32];
#ifdef CCORD_HAVE_DAVE
  DAVEDecryptorHandle decryptor;
#endif
} dave_peer;

struct ccord_dave {
  ccord_id self_id;
  ccord_id group_id;
  ccord_dave_send_fn send;
  void *send_ud;
  char self_user_str[32];
  uint16_t latest_prepared_version;
  dave_transition transitions[DAVE_MAX_TRANSITIONS];
  size_t next_transition_slot;
  dave_peer *peers;
  size_t npeers;
  size_t cap_peers;
#ifdef CCORD_HAVE_DAVE
  DAVESessionHandle session;
  DAVEEncryptorHandle encryptor;
#endif
};

bool ccord_dave_built(void)
{
#ifdef CCORD_HAVE_DAVE
  return true;
#else
  return false;
#endif
}

uint16_t ccord_dave_max_protocol_version(void)
{
#ifdef CCORD_HAVE_DAVE
  return daveMaxSupportedProtocolVersion();
#else
  return 0;
#endif
}

#ifdef CCORD_HAVE_DAVE
static void dave_mls_failure_cb(const char *source, const char *reason, void *user_data)
{
  (void)user_data;
  CCORD_LOG_W("dave: MLS failure in %s: %s", source ? source : "?", reason ? reason : "?");
}

static DAVEMediaType to_dave_media(ccord_dave_media m)
{
  return m == CCORD_DAVE_VIDEO ? DAVE_MEDIA_TYPE_VIDEO : DAVE_MEDIA_TYPE_AUDIO;
}

static DAVECodec to_dave_codec(ccord_dave_codec c)
{
  switch (c) {
    case CCORD_DAVE_OPUS: return DAVE_CODEC_OPUS;
    case CCORD_DAVE_H264: return DAVE_CODEC_H264;
  }
  return DAVE_CODEC_UNKNOWN;
}

static void dave_send_json(ccord_dave *d, const char *json, size_t len)
{
  if (!d->send) return;
  ccord_error err;
  (void)d->send(d->send_ud, false, (const uint8_t *)json, len, &err);
}

static void dave_send_bin_op(ccord_dave *d, uint8_t op, const uint8_t *payload, size_t len)
{
  if (!d->send) return;
  uint8_t stack_buf[2048];
  uint8_t *buf = stack_buf;
  size_t cap = sizeof stack_buf;
  if (len + 1 > cap) {
    cap = len + 1;
    buf = ccord_malloc(cap);
    if (!buf) return;
  }
  size_t flen = ccord_vframe_build(op, payload, len, buf, cap);
  ccord_error err;
  if (flen > 0) (void)d->send(d->send_ud, true, buf, flen, &err);
  if (buf != stack_buf) ccord_free(buf);
}

static void dave_send_ready_for_transition(ccord_dave *d, uint16_t transition_id)
{
  if (transition_id == DAVE_INIT_TRANSITION_ID) return;
  char json[96];
  int n = snprintf(json, sizeof json, "{\"op\":%d,\"d\":{\"transition_id\":%u}}",
                   CCORD_VOICE_OP_DAVE_TRANSITION_READY, (unsigned)transition_id);
  if (n > 0 && (size_t)n < sizeof json) {
    dave_send_json(d, json, (size_t)n);
  }
}

static void dave_send_invalid_commit_welcome(ccord_dave *d, uint16_t transition_id)
{
  char json[96];
  int n = snprintf(json, sizeof json, "{\"op\":%d,\"d\":{\"transition_id\":%u}}",
                   CCORD_VOICE_OP_DAVE_INVALID_COMMIT, (unsigned)transition_id);
  if (n > 0 && (size_t)n < sizeof json) {
    dave_send_json(d, json, (size_t)n);
  }
}

static void dave_send_key_package(ccord_dave *d)
{
  uint8_t *kp = nullptr;
  size_t kp_len = 0;
  daveSessionGetMarshalledKeyPackage(d->session, &kp, &kp_len);
  if (kp && kp_len > 0) {
    dave_send_bin_op(d, (uint8_t)CCORD_VOICE_OP_DAVE_KEY_PACKAGE, kp, kp_len);
  }
  daveFree(kp);
}

static void dave_setup_self_ratchet(ccord_dave *d, uint16_t version)
{
  if (version == 0) {
    daveEncryptorSetPassthroughMode(d->encryptor, true);
    daveEncryptorSetKeyRatchet(d->encryptor, nullptr);
  } else {
    DAVEKeyRatchetHandle r = daveSessionGetKeyRatchet(d->session, d->self_user_str);
    daveEncryptorSetKeyRatchet(d->encryptor, r);
    daveEncryptorSetPassthroughMode(d->encryptor, false);
    if (r) daveKeyRatchetDestroy(r);
  }
}

static void dave_setup_peer_ratchet(ccord_dave *d, dave_peer *p, uint16_t version)
{
  if (!p || !p->decryptor) return;
  if (version == 0) {
    daveDecryptorTransitionToPassthroughMode(p->decryptor, true);
    daveDecryptorTransitionToKeyRatchet(p->decryptor, nullptr);
  } else {
    DAVEKeyRatchetHandle r = daveSessionGetKeyRatchet(d->session, p->user_str);
    daveDecryptorTransitionToKeyRatchet(p->decryptor, r);
    daveDecryptorTransitionToPassthroughMode(p->decryptor, false);
    if (r) daveKeyRatchetDestroy(r);
  }
}

static void dave_store_transition(ccord_dave *d, uint16_t transition_id, uint16_t version)
{
  for (size_t i = 0; i < DAVE_MAX_TRANSITIONS; i++) {
    if (d->transitions[i].used && d->transitions[i].id == transition_id) {
      d->transitions[i].version = version;
      return;
    }
  }
  for (size_t i = 0; i < DAVE_MAX_TRANSITIONS; i++) {
    if (!d->transitions[i].used) {
      d->transitions[i] = (dave_transition){.id = transition_id, .version = version, .used = true};
      return;
    }
  }
  size_t slot = d->next_transition_slot % DAVE_MAX_TRANSITIONS;
  d->next_transition_slot++;
  d->transitions[slot] = (dave_transition){.id = transition_id, .version = version, .used = true};
}

static void dave_prepare_ratchets(ccord_dave *d, uint16_t transition_id, uint16_t version)
{
  for (size_t i = 0; i < d->npeers; i++) {
    dave_setup_peer_ratchet(d, &d->peers[i], version);
  }
  if (transition_id == DAVE_INIT_TRANSITION_ID) {
    dave_setup_self_ratchet(d, version);
  } else {
    dave_store_transition(d, transition_id, version);
  }
  d->latest_prepared_version = version;
}

static void dave_execute_transition_internal(ccord_dave *d, uint16_t transition_id)
{
  uint16_t version = 0;
  bool found = false;
  for (size_t i = 0; i < DAVE_MAX_TRANSITIONS; i++) {
    if (d->transitions[i].used && d->transitions[i].id == transition_id) {
      version = d->transitions[i].version;
      d->transitions[i].used = false;
      found = true;
      break;
    }
  }
  if (!found) return;
  if (version == 0) daveSessionReset(d->session);
  dave_setup_self_ratchet(d, version);
}

static void dave_prepare_epoch_internal(ccord_dave *d, uint32_t epoch, uint16_t version)
{
  if (epoch == DAVE_NEW_GROUP_EPOCH) {
    daveSessionInit(d->session, version, (uint64_t)d->group_id, d->self_user_str);
    d->latest_prepared_version = version;
  }
}

static void dave_init_protocol(ccord_dave *d, uint16_t version)
{
  uint16_t max_ver = daveMaxSupportedProtocolVersion();
  if (version > max_ver) version = max_ver;
  if (version > 0) {
    daveEncryptorSetPassthroughMode(d->encryptor, false);
    dave_prepare_epoch_internal(d, DAVE_NEW_GROUP_EPOCH, version);
    dave_send_key_package(d);
    return;
  }
  dave_prepare_ratchets(d, DAVE_INIT_TRANSITION_ID, 0);
  dave_execute_transition_internal(d, DAVE_INIT_TRANSITION_ID);
}

static dave_peer *dave_find_or_add_peer(ccord_dave *d, ccord_id user_id, bool create)
{
  if (user_id == CCORD_ID_NONE || user_id == d->self_id) return nullptr;
  for (size_t i = 0; i < d->npeers; i++) {
    if (d->peers[i].user_id == user_id) return &d->peers[i];
  }
  if (!create) return nullptr;
  if (d->npeers == d->cap_peers) {
    size_t new_cap = d->cap_peers ? d->cap_peers * 2 : 16;
    dave_peer *np = ccord_realloc(d->peers, new_cap * sizeof *np);
    if (!np) return nullptr;
    d->peers = np;
    d->cap_peers = new_cap;
  }
  dave_peer *p = &d->peers[d->npeers++];
  memset(p, 0, sizeof *p);
  p->user_id = user_id;
  (void)snprintf(p->user_str, sizeof p->user_str, "%llu", (unsigned long long)user_id);
  p->decryptor = daveDecryptorCreate();
  if (p->decryptor) {
    daveDecryptorTransitionToPassthroughMode(p->decryptor, true);
  }
  dave_setup_peer_ratchet(d, p, d->latest_prepared_version);
  return p;
}

static const char **dave_build_recognized_ids(const ccord_dave *d, size_t *out_count)
{
  size_t total = d->npeers + 1;
  const char **arr = ccord_calloc(total, sizeof *arr);
  if (!arr) {
    *out_count = 0;
    return nullptr;
  }
  for (size_t i = 0; i < d->npeers; i++) {
    arr[i] = d->peers[i].user_str;
  }
  arr[d->npeers] = d->self_user_str;
  *out_count = total;
  return arr;
}
#endif

ccord_dave *ccord_dave_create(ccord_id self_id,
                              ccord_id group_id,
                              ccord_dave_send_fn send,
                              void *send_ud,
                              ccord_error *err)
{
  ccord_error_clear(err);
#ifdef CCORD_HAVE_DAVE
  if (self_id == CCORD_ID_NONE || group_id == CCORD_ID_NONE) {
    ccord_error_set(err, CCORD_ERR_INVALID, "self_id and group_id must be non-zero");
    return nullptr;
  }
  ccord_dave *d = ccord_calloc(1, sizeof *d);
  if (!d) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "out of memory creating DAVE state");
    return nullptr;
  }
  d->self_id = self_id;
  d->group_id = group_id;
  d->send = send;
  d->send_ud = send_ud;
  (void)snprintf(d->self_user_str, sizeof d->self_user_str, "%llu", (unsigned long long)self_id);
  d->session = daveSessionCreate(nullptr, nullptr, dave_mls_failure_cb, d);
  d->encryptor = daveEncryptorCreate();
  if (!d->session || !d->encryptor) {
    ccord_dave_destroy(d);
    ccord_error_set(err, CCORD_ERR_NOMEM, "failed to create libdave session/encryptor");
    return nullptr;
  }
  daveEncryptorSetPassthroughMode(d->encryptor, true);
  return d;
#else
  (void)self_id;
  (void)group_id;
  (void)send;
  (void)send_ud;
  ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "libdave support not compiled");
  return nullptr;
#endif
}

void ccord_dave_destroy(ccord_dave *d)
{
  if (!d) return;
#ifdef CCORD_HAVE_DAVE
  for (size_t i = 0; i < d->npeers; i++) {
    if (d->peers[i].decryptor) daveDecryptorDestroy(d->peers[i].decryptor);
  }
  if (d->encryptor) daveEncryptorDestroy(d->encryptor);
  if (d->session) daveSessionDestroy(d->session);
#endif
  ccord_free(d->peers);
  ccord_free(d);
}

void ccord_dave_on_session_description(ccord_dave *d, uint16_t protocol_version)
{
  if (!d) return;
#ifdef CCORD_HAVE_DAVE
  dave_init_protocol(d, protocol_version);
#else
  (void)protocol_version;
#endif
}

void ccord_dave_on_clients_connect(ccord_dave *d, const ccord_id *user_ids, size_t count)
{
  if (!d || (count > 0 && !user_ids)) return;
#ifdef CCORD_HAVE_DAVE
  for (size_t i = 0; i < count; i++) {
    if (user_ids[i] == CCORD_ID_NONE || user_ids[i] == d->self_id) continue;
    (void)dave_find_or_add_peer(d, user_ids[i], true);
  }
#endif
}

void ccord_dave_on_client_disconnect(ccord_dave *d, ccord_id user_id)
{
  if (!d || user_id == CCORD_ID_NONE) return;
#ifdef CCORD_HAVE_DAVE
  for (size_t i = 0; i < d->npeers; i++) {
    if (d->peers[i].user_id == user_id) {
      if (d->peers[i].decryptor) daveDecryptorDestroy(d->peers[i].decryptor);
      if (i + 1 < d->npeers) {
        memmove(&d->peers[i], &d->peers[i + 1], (d->npeers - i - 1) * sizeof *d->peers);
      }
      d->npeers--;
      return;
    }
  }
#endif
}

void ccord_dave_on_prepare_transition(ccord_dave *d, uint16_t transition_id, uint16_t protocol_version)
{
  if (!d) return;
#ifdef CCORD_HAVE_DAVE
  uint16_t max_ver = daveMaxSupportedProtocolVersion();
  if (protocol_version > max_ver) protocol_version = max_ver;
  dave_prepare_ratchets(d, transition_id, protocol_version);
  dave_send_ready_for_transition(d, transition_id);
#else
  (void)transition_id;
  (void)protocol_version;
#endif
}

void ccord_dave_on_execute_transition(ccord_dave *d, uint16_t transition_id)
{
  if (!d) return;
#ifdef CCORD_HAVE_DAVE
  dave_execute_transition_internal(d, transition_id);
#else
  (void)transition_id;
#endif
}

void ccord_dave_on_prepare_epoch(ccord_dave *d, uint32_t epoch, uint16_t protocol_version)
{
  if (!d) return;
#ifdef CCORD_HAVE_DAVE
  uint16_t max_ver = daveMaxSupportedProtocolVersion();
  if (protocol_version > max_ver) protocol_version = max_ver;
  dave_prepare_epoch_internal(d, epoch, protocol_version);
  if (epoch == DAVE_NEW_GROUP_EPOCH) {
    dave_send_key_package(d);
  }
#else
  (void)epoch;
  (void)protocol_version;
#endif
}

void ccord_dave_on_external_sender(ccord_dave *d, const uint8_t *data, size_t len)
{
  if (!d || !data || len == 0) return;
#ifdef CCORD_HAVE_DAVE
  daveSessionSetExternalSender(d->session, data, len);
#endif
}

void ccord_dave_on_proposals(ccord_dave *d, const uint8_t *data, size_t len)
{
  if (!d || !data || len == 0) return;
#ifdef CCORD_HAVE_DAVE
  size_t rec_count = 0;
  const char **rec_ids = dave_build_recognized_ids(d, &rec_count);
  if (!rec_ids) return;
  uint8_t *cw = nullptr;
  size_t cw_len = 0;
  daveSessionProcessProposals(d->session, data, len, rec_ids, rec_count, &cw, &cw_len);
  ccord_free(rec_ids);
  if (cw && cw_len > 0) {
    dave_send_bin_op(d, (uint8_t)CCORD_VOICE_OP_DAVE_COMMIT_WELCOME, cw, cw_len);
  }
  daveFree(cw);
#endif
}

void ccord_dave_on_announce_commit(ccord_dave *d, uint16_t transition_id, const uint8_t *commit, size_t len)
{
  if (!d || !commit || len == 0) return;
#ifdef CCORD_HAVE_DAVE
  DAVECommitResultHandle res = daveSessionProcessCommit(d->session, commit, len);
  if (!res) {
    dave_send_invalid_commit_welcome(d, transition_id);
    dave_init_protocol(d, daveSessionGetProtocolVersion(d->session));
    return;
  }
  bool ignored = daveCommitResultIsIgnored(res);
  bool failed = daveCommitResultIsFailed(res);
  if (!ignored && !failed) {
    uint64_t *roster_ids = nullptr;
    size_t roster_len = 0;
    daveCommitResultGetRosterMemberIds(res, &roster_ids, &roster_len);
    for (size_t i = 0; i < roster_len; i++) {
      (void)dave_find_or_add_peer(d, (ccord_id)roster_ids[i], true);
    }
    daveFree(roster_ids);
  }
  daveCommitResultDestroy(res);
  if (ignored) return;
  if (!failed) {
    uint16_t ver = daveSessionGetProtocolVersion(d->session);
    dave_prepare_ratchets(d, transition_id, ver);
    dave_send_ready_for_transition(d, transition_id);
    return;
  }
  dave_send_invalid_commit_welcome(d, transition_id);
  dave_init_protocol(d, daveSessionGetProtocolVersion(d->session));
#else
  (void)transition_id;
#endif
}

void ccord_dave_on_welcome(ccord_dave *d, uint16_t transition_id, const uint8_t *welcome, size_t len)
{
  if (!d || !welcome || len == 0) return;
#ifdef CCORD_HAVE_DAVE
  size_t rec_count = 0;
  const char **rec_ids = dave_build_recognized_ids(d, &rec_count);
  if (!rec_ids) return;
  DAVEWelcomeResultHandle res = daveSessionProcessWelcome(d->session, welcome, len, rec_ids, rec_count);
  ccord_free(rec_ids);
  if (res) {
    uint64_t *roster_ids = nullptr;
    size_t roster_len = 0;
    daveWelcomeResultGetRosterMemberIds(res, &roster_ids, &roster_len);
    for (size_t i = 0; i < roster_len; i++) {
      (void)dave_find_or_add_peer(d, (ccord_id)roster_ids[i], true);
    }
    daveFree(roster_ids);
    daveWelcomeResultDestroy(res);
    uint16_t ver = daveSessionGetProtocolVersion(d->session);
    dave_prepare_ratchets(d, transition_id, ver);
    dave_send_ready_for_transition(d, transition_id);
    return;
  }
  dave_send_invalid_commit_welcome(d, transition_id);
  dave_send_key_package(d);
#else
  (void)transition_id;
#endif
}

void ccord_dave_assign_ssrc(ccord_dave *d, uint32_t ssrc, ccord_dave_codec codec)
{
  if (!d) return;
#ifdef CCORD_HAVE_DAVE
  daveEncryptorAssignSsrcToCodec(d->encryptor, ssrc, to_dave_codec(codec));
#else
  (void)ssrc;
  (void)codec;
#endif
}

size_t ccord_dave_max_ciphertext(ccord_dave *d, ccord_dave_media media, size_t len)
{
#ifdef CCORD_HAVE_DAVE
  if (d && d->encryptor) {
    size_t sz = daveEncryptorGetMaxCiphertextByteSize(d->encryptor, to_dave_media(media), len);
    if (sz >= len) return sz;
  }
#else
  (void)d;
  (void)media;
#endif
  return len + 64;
}

ccord_dave_result ccord_dave_encrypt(ccord_dave *d,
                                     ccord_dave_media media,
                                     uint32_t ssrc,
                                     const uint8_t *in,
                                     size_t len,
                                     uint8_t *out,
                                     size_t cap,
                                     size_t *out_len)
{
  if (out_len) *out_len = 0;
  if (!d || !in || !out || !out_len || len == 0) return CCORD_DAVE_FAILED;
#ifdef CCORD_HAVE_DAVE
  size_t written = 0;
  DAVEEncryptorResultCode rc =
    daveEncryptorEncrypt(d->encryptor, to_dave_media(media), ssrc, in, len, out, cap, &written);
  if (rc == DAVE_ENCRYPTOR_RESULT_CODE_SUCCESS) {
    *out_len = written;
    return CCORD_DAVE_OK;
  }
  if (rc == DAVE_ENCRYPTOR_RESULT_CODE_MISSING_KEY_RATCHET || rc == DAVE_ENCRYPTOR_RESULT_CODE_MISSING_CRYPTOR) {
    return CCORD_DAVE_NOT_READY;
  }
  return CCORD_DAVE_FAILED;
#else
  (void)media;
  (void)ssrc;
  if (cap < len) return CCORD_DAVE_FAILED;
  if (out != in) memcpy(out, in, len);
  *out_len = len;
  return CCORD_DAVE_OK;
#endif
}

ccord_dave_result ccord_dave_decrypt(ccord_dave *d,
                                     ccord_id sender,
                                     ccord_dave_media media,
                                     const uint8_t *in,
                                     size_t len,
                                     uint8_t *out,
                                     size_t cap,
                                     size_t *out_len)
{
  if (out_len) *out_len = 0;
  if (!d || !in || !out || !out_len || len == 0) return CCORD_DAVE_FAILED;
#ifdef CCORD_HAVE_DAVE
  bool has_dave_marker = (len >= 4u && in[len - 2u] == 0xFAu && in[len - 1u] == 0xFAu);
  dave_peer *p = dave_find_or_add_peer(d, sender, true);
  if ((!p || !p->decryptor) && sender == CCORD_ID_NONE && d->npeers == 1u) {
    p = &d->peers[0];
  }
  if (!p || !p->decryptor) {
    if (!ccord_dave_e2ee_active(d) && !has_dave_marker) {
      if (cap < len) return CCORD_DAVE_FAILED;
      if (out != in) memcpy(out, in, len);
      *out_len = len;
      return CCORD_DAVE_OK;
    }
    return CCORD_DAVE_NOT_READY;
  }
  size_t written = 0;
  DAVEDecryptorResultCode rc =
    daveDecryptorDecrypt(p->decryptor, to_dave_media(media), in, len, out, cap, &written);
  if (rc == DAVE_DECRYPTOR_RESULT_CODE_SUCCESS) {
    *out_len = written;
    return CCORD_DAVE_OK;
  }
  if (rc == DAVE_DECRYPTOR_RESULT_CODE_MISSING_KEY_RATCHET || rc == DAVE_DECRYPTOR_RESULT_CODE_MISSING_CRYPTOR) {
    return CCORD_DAVE_NOT_READY;
  }
  return CCORD_DAVE_FAILED;
#else
  (void)sender;
  (void)media;
  if (cap < len) return CCORD_DAVE_FAILED;
  if (out != in) memcpy(out, in, len);
  *out_len = len;
  return CCORD_DAVE_OK;
#endif
}

void ccord_dave_get_privacy_code(ccord_dave *d, char *out, size_t cap)
{
  if (!out || cap == 0) return;
  out[0] = '\0';
#ifdef CCORD_HAVE_DAVE
  if (!d || !d->session) return;
  uint8_t *auth = NULL;
  size_t auth_len = 0;
  daveSessionGetLastEpochAuthenticator(d->session, &auth, &auth_len);
  if (auth && auth_len >= 30) {
    uint64_t vals[6];
    for (int i = 0; i < 6; i++) {
      const uint8_t *p = auth + (i * 5);
      vals[i] = (((uint64_t)p[0] << 32) | ((uint64_t)p[1] << 24) | ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 8) | (uint64_t)p[4]) % 100000ULL;
    }
    snprintf(out, cap, "%05u %05u %05u %05u %05u %05u",
             (unsigned)vals[0], (unsigned)vals[1], (unsigned)vals[2],
             (unsigned)vals[3], (unsigned)vals[4], (unsigned)vals[5]);
  }
  if (auth) {
    daveFree(auth);
  }
#else
  (void)d;
#endif
}

uint16_t ccord_dave_protocol_version(const ccord_dave *d)
{
  return d ? d->latest_prepared_version : 0;
}

bool ccord_dave_e2ee_active(const ccord_dave *d)
{
#ifdef CCORD_HAVE_DAVE
  return d && d->encryptor && !daveEncryptorIsPassthroughMode(d->encryptor);
#else
  (void)d;
  return false;
#endif
}

bool ccord_dave_can_send(const ccord_dave *d)
{
  if (!d) return false;
#ifdef CCORD_HAVE_DAVE
  if (!d->encryptor) return false;
  return daveEncryptorIsPassthroughMode(d->encryptor) || daveEncryptorHasKeyRatchet(d->encryptor);
#else
  return true;
#endif
}

size_t ccord_dave_peers(const ccord_dave *d, ccord_id *out_ids, size_t cap)
{
  if (!d) return 0;
  size_t n = d->npeers < cap ? d->npeers : cap;
  for (size_t i = 0; i < n && out_ids; i++) out_ids[i] = d->peers[i].user_id;
  return d->npeers;
}
