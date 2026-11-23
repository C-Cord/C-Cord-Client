// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// The structure of a message beyond its text: what Discord sends as attachments, embeds, stickers, mentions, the
// message a reply points at, a forwarded snapshot, a poll and a started thread. A plain text message has none of it
// (ccord_message.rich == nullptr), so the common case costs nothing.
//
// A block that is stored (a clone) never holds a null string: an absent text is "". A block a producer builds may leave
// strings null, which means the same. Everything a stored block points to lives in the block itself (an arena owned by
// it), so one ccord_message_rich_free releases it all and a clone is a deep copy.
#pragma once
#include <ccord/model/model.h>

#define CCORD_RICH_MAX_ATTACHMENTS 10u
#define CCORD_RICH_MAX_EMBEDS 10u
#define CCORD_RICH_MAX_EMBED_FIELDS 25u
#define CCORD_RICH_MAX_STICKERS 3u
#define CCORD_RICH_MAX_MENTIONS 100u            // users, and separately roles
#define CCORD_RICH_MAX_POLL_ANSWERS 10u
#define CCORD_RICH_MAX_COMPONENT_ROWS 5u
#define CCORD_RICH_MAX_ROW_COMPONENTS 5u
#define CCORD_RICH_MAX_SELECT_OPTIONS 25u
#define CCORD_RICH_MAX_V2_NODES 40u             // Components v2: every node of the tree, nested ones included
#define CCORD_RICH_MAX_V2_DEPTH 4u              // a top-level component is depth 1; an accessory is one deeper than its section
#define CCORD_RICH_MAX_V2_SECTION_TEXTS 3u
#define CCORD_RICH_MAX_V2_GALLERY_ITEMS 10u
#define CCORD_RICH_CUSTOM_ID_MAX_BYTES 100u
#define CCORD_RICH_SHORT_MAX_BYTES 256u         // names, titles, filenames, labels
#define CCORD_RICH_FIELD_VALUE_MAX_BYTES 1024u
#define CCORD_RICH_TEXT_MAX_BYTES 4096u         // embed description, forwarded text
#define CCORD_RICH_URL_MAX_BYTES 2048u
#define CCORD_RICH_WAVEFORM_MAX_BYTES 512u      // voice-message waveform, base64 text (Discord sends ~344 for 256 bytes)
#define CCORD_RICH_SNIPPET_MAX_BYTES 300u       // what a reply preview keeps of the replied-to text

typedef struct ccord_attachment {
  ccord_id id;
  const char *filename;      // as uploaded; "SPOILER_" prefix marks a spoiler
  const char *description;   // alt text
  const char *content_type;  // MIME type, "" when Discord did not say
  const char *url;
  const char *proxy_url;
  const char *waveform_b64;  // voice message waveform, base64 text; "" when Discord sent none
  uint64_t size;             // bytes
  uint32_t width, height;    // pixels; 0 when it is not an image or video
  uint32_t duration_ms;      // audio / video / voice message; 0 when unknown
  uint32_t flags;            // Discord attachment flags (1 << 2 is_remix)
  bool spoiler;              // "SPOILER_" filename prefix
} ccord_attachment;

typedef enum ccord_embed_kind : uint8_t {
  CCORD_EMBED_RICH = 0, CCORD_EMBED_IMAGE, CCORD_EMBED_VIDEO, CCORD_EMBED_GIFV, CCORD_EMBED_ARTICLE, CCORD_EMBED_LINK,
  CCORD_EMBED_POLL_RESULT, CCORD_EMBED_OTHER,
} ccord_embed_kind;
typedef struct ccord_embed_media {
  const char *url, *proxy_url;   // "" when the embed has no such media
  uint32_t width, height;
} ccord_embed_media;
typedef struct ccord_embed_field {
  const char *name, *value;
  bool inline_;                  // side by side with its neighbours
} ccord_embed_field;
typedef struct ccord_embed {
  const char *title, *description, *url;
  const char *provider;
  const char *author_name, *author_url, *author_icon;
  const char *footer_text, *footer_icon;
  int64_t timestamp_ms;          // 0 = none
  ccord_embed_media image, thumbnail, video;
  const ccord_embed_field *fields; size_t field_count;
  uint32_t color;                // 0xRRGGBB, meaningful when has_color
  ccord_embed_kind kind;
  bool has_color;
} ccord_embed;

typedef struct ccord_msg_sticker {
  ccord_id id;
  const char *name;
  uint8_t format;                // Discord format_type: 1 PNG, 2 APNG, 3 Lottie, 4 GIF; 0 unknown
} ccord_msg_sticker;

typedef enum ccord_ref_kind : uint8_t { CCORD_REF_NONE = 0, CCORD_REF_REPLY, CCORD_REF_FORWARD } ccord_ref_kind;
typedef struct ccord_msg_reference {
  ccord_ref_kind kind;
  ccord_id message_id, channel_id, guild_id;   // zero where Discord left them out
} ccord_msg_reference;
// The message a reply points at, as Discord attached it (a snippet, enough for the line above the reply).
typedef struct ccord_ref_preview {
  ccord_id author_id;
  const char *author_name;       // display name, "" when unknown
  const char *content;           // first CCORD_RICH_SNIPPET_MAX_BYTES of its text
  bool present;                  // Discord sent the replied-to message
  bool deleted;                  // it said the message no longer exists
  bool has_attachments;          // it carried files (shown as an icon when it has no text)
} ccord_ref_preview;

// A forwarded message: Discord sends a snapshot of the original inside the new one.
typedef struct ccord_forward {
  const char *content;
  int64_t timestamp_ms;
  const ccord_attachment *attachments; size_t attachment_count;
  const ccord_embed *embeds; size_t embed_count;
  const ccord_msg_sticker *stickers; size_t sticker_count;
  bool present;
} ccord_forward;

typedef struct ccord_poll_answer {
  const char *text;
  const char *emoji;             // Unicode emoji or custom emoji name, "" when none
  ccord_id emoji_id;
  uint32_t id;                   // answer_id
  uint32_t votes;
  bool me_voted;
} ccord_poll_answer;
typedef struct ccord_poll {
  const char *question;
  ccord_poll_answer *answers;    // up to CCORD_RICH_MAX_POLL_ANSWERS (mutable for in-place vote updates)
  size_t answer_count;
  int64_t expiry_ms;             // 0 = none
  bool present;
  bool multiselect;
  bool finalized;                // voting is over and the counts are final
} ccord_poll;

typedef struct ccord_msg_thread {
  ccord_id id;                   // 0 = no thread was started from this message
  const char *name;
  uint32_t message_count;
} ccord_msg_thread;

typedef enum ccord_component_kind : uint8_t {
  CCORD_COMP_ACTION_ROW = 1,
  CCORD_COMP_BUTTON = 2,
  CCORD_COMP_STRING_SELECT = 3,
  CCORD_COMP_TEXT_INPUT = 4,
  CCORD_COMP_USER_SELECT = 5,
  CCORD_COMP_ROLE_SELECT = 6,
  CCORD_COMP_MENTIONABLE_SELECT = 7,
  CCORD_COMP_CHANNEL_SELECT = 8,
  // Components v2 (message flag CCORD_MSGFLAG_IS_COMPONENTS_V2): layout and content components.
  CCORD_COMP_SECTION = 9,          // 1-3 TextDisplay children and an accessory (Thumbnail or Button)
  CCORD_COMP_TEXT_DISPLAY = 10,    // markdown text
  CCORD_COMP_THUMBNAIL = 11,       // a small image, only as a Section accessory
  CCORD_COMP_MEDIA_GALLERY = 12,   // 1-10 images / videos
  CCORD_COMP_FILE = 13,            // an uploaded file, referenced as attachment://name
  CCORD_COMP_SEPARATOR = 14,       // spacing and an optional divider line
  CCORD_COMP_CONTAINER = 17,       // groups components behind an accent colour bar
} ccord_component_kind;

typedef enum ccord_button_style : uint8_t {
  CCORD_BTN_PRIMARY = 1,
  CCORD_BTN_SECONDARY = 2,
  CCORD_BTN_SUCCESS = 3,
  CCORD_BTN_DANGER = 4,
  CCORD_BTN_LINK = 5,
  CCORD_BTN_PREMIUM = 6,
} ccord_button_style;

typedef struct ccord_select_option {
  const char *label;
  const char *value;
  const char *description;
  const char *emoji;
  ccord_id emoji_id;
  bool default_;
} ccord_select_option;

// An unfurled media item of a v2 component. `url` is an http(s) URL or "attachment://<filename>" naming one of the
// message's attachments; use ccord_rich_media_url to resolve it. A producer may leave strings null (same as "").
typedef struct ccord_media_ref {
  const char *url;
  const char *proxy_url;         // the CDN's copy, set by Discord
  uint32_t width, height;        // 0 when unknown
  const char *content_type;
  const char *description;       // alt text (Thumbnail, gallery items)
  bool spoiler;                  // Thumbnail and gallery items; a File or Container uses ccord_component.spoiler
} ccord_media_ref;

// One component. The v1 kinds (ACTION_ROW .. CHANNEL_SELECT) use the fields up to `option_count`; v1 rows are stored as
// ccord_component_row, so a v1 component is a leaf. The v2 kinds form a tree: `children` (Container, Section, and
// ACTION_ROW inside a v2 tree) and `accessory` (Section) point at further nodes, all owned by the rich block.
typedef struct ccord_component {
  ccord_component_kind kind;
  uint8_t style;                 // ccord_button_style for BUTTON; 1 short / 2 paragraph for TEXT_INPUT
  const char *label;
  const char *custom_id;
  const char *url;               // LINK button target
  const char *placeholder;       // select menu / text input
  const char *emoji;
  ccord_id emoji_id;
  bool disabled;
  uint8_t min_values;            // select menu (0..25)
  uint8_t max_values;            // select menu (1..25; 0 means 1)
  const ccord_select_option *options;
  size_t option_count;
  // Components v2.
  const struct ccord_component *children; size_t child_count;   // Container, Section, v2 ACTION_ROW
  const struct ccord_component *accessory;                      // Section: a Thumbnail or Button, or nullptr
  const char *content;           // TEXT_DISPLAY markdown
  ccord_media_ref media;         // THUMBNAIL, FILE
  const ccord_media_ref *gallery; size_t gallery_count;         // MEDIA_GALLERY items
  uint32_t accent_color;         // CONTAINER 0xRRGGBB, meaningful when has_accent_color
  bool has_accent_color;
  bool spoiler;                  // CONTAINER, FILE
  bool divider;                  // SEPARATOR draws a line (Discord defaults to true; a producer must set it)
  uint8_t spacing;               // SEPARATOR padding: 1 small (0 reads as 1), 2 large
  const char *file_name;         // FILE: the name Discord reports
  uint64_t file_size;            // FILE: bytes
} ccord_component;

typedef struct ccord_component_row {
  const ccord_component *components;
  size_t component_count;
} ccord_component_row;

// Modal prompt pushed by INTERACTION_MODAL_CREATE in answer to a component or slash-command interaction.
typedef struct ccord_modal_field {
  char custom_id[CCORD_RICH_CUSTOM_ID_MAX_BYTES + 1];
  char label[CCORD_RICH_SHORT_MAX_BYTES + 1];
  char placeholder[CCORD_RICH_SHORT_MAX_BYTES + 1];
  char value[CCORD_RICH_TEXT_MAX_BYTES + 1];
  uint8_t style;                 // 1 = short single-line, 2 = paragraph multi-line
  uint16_t min_length;
  uint16_t max_length;           // 0 = default (4000)
  bool required;
} ccord_modal_field;

typedef struct ccord_interaction_modal {
  ccord_id id;                   // interaction snowflake
  ccord_id application_id;
  ccord_id guild_id;
  ccord_id channel_id;
  char custom_id[CCORD_RICH_CUSTOM_ID_MAX_BYTES + 1];
  char title[CCORD_RICH_SHORT_MAX_BYTES + 1];
  ccord_modal_field fields[CCORD_RICH_MAX_COMPONENT_ROWS];
  size_t field_count;
} ccord_interaction_modal;

#define CCORD_SLASH_NAME_MAX_BYTES 32u
#define CCORD_SLASH_DESC_MAX_BYTES 100u
#define CCORD_SLASH_CHOICE_MAX_BYTES 100u
#define CCORD_SLASH_MAX_OPTIONS 25u
#define CCORD_SLASH_MAX_CHOICES 25u
#define CCORD_SLASH_MAX_COMMANDS 128u

typedef enum ccord_slash_opt_kind : uint8_t {
  CCORD_SLASH_OPT_SUB_COMMAND = 1,
  CCORD_SLASH_OPT_SUB_COMMAND_GROUP = 2,
  CCORD_SLASH_OPT_STRING = 3,
  CCORD_SLASH_OPT_INTEGER = 4,
  CCORD_SLASH_OPT_BOOLEAN = 5,
  CCORD_SLASH_OPT_USER = 6,
  CCORD_SLASH_OPT_CHANNEL = 7,
  CCORD_SLASH_OPT_ROLE = 8,
  CCORD_SLASH_OPT_MENTIONABLE = 9,
  CCORD_SLASH_OPT_NUMBER = 10,
} ccord_slash_opt_kind;

typedef struct ccord_slash_option {
  ccord_slash_opt_kind kind;
  char name[CCORD_SLASH_NAME_MAX_BYTES + 1];
  char description[CCORD_SLASH_DESC_MAX_BYTES + 1];
  bool required;
  bool autocomplete;
  uint8_t parent_plus_one;       // 0 = root option; k = child of options[k-1] (a SUB_COMMAND_GROUP / SUB_COMMAND)
} ccord_slash_option;

typedef struct ccord_slash_command {
  ccord_id id;
  ccord_id application_id;
  ccord_id guild_id;             // 0 = global command
  ccord_id version;              // Discord command version snowflake (0 -> id)
  uint64_t default_member_permissions;  // UINT64_MAX = unrestricted, else required permission mask
  const ccord_slash_option *options;
  size_t option_count;
  char name[CCORD_SLASH_NAME_MAX_BYTES + 1];
  char description[CCORD_SLASH_DESC_MAX_BYTES + 1];
  bool dm_permission;
  uint8_t command_type;          // 0/1 = CHAT_INPUT (slash), 2 = USER, 3 = MESSAGE context-menu command
} ccord_slash_command;

typedef struct ccord_slash_choice {
  char name[CCORD_SLASH_CHOICE_MAX_BYTES + 1];
  char value[CCORD_SLASH_CHOICE_MAX_BYTES + 1];
} ccord_slash_choice;

// Which parts of a rich block a MESSAGE_UPDATE actually carried. Discord sends partial updates (a link preview
// arriving later has only the embeds), so a part that is not "present" keeps what the stored message had.
enum {
  CCORD_RICH_HAS_TYPE = 1u << 0,
  CCORD_RICH_HAS_MENTIONS = 1u << 1,
  CCORD_RICH_HAS_ATTACHMENTS = 1u << 2,
  CCORD_RICH_HAS_EMBEDS = 1u << 3,
  CCORD_RICH_HAS_STICKERS = 1u << 4,
  CCORD_RICH_HAS_REFERENCE = 1u << 5,
  CCORD_RICH_HAS_FORWARD = 1u << 6,
  CCORD_RICH_HAS_POLL = 1u << 7,
  CCORD_RICH_HAS_THREAD = 1u << 8,
  CCORD_RICH_HAS_COMPONENTS = 1u << 9,
  CCORD_RICH_HAS_FLAGS = 1u << 10,
  CCORD_RICH_ALL = (1u << 11) - 1u,
};

// Discord message flags (the `flags` field).
#define CCORD_MSGFLAG_CROSSPOSTED (1u << 0)
#define CCORD_MSGFLAG_IS_CROSSPOST (1u << 1)
#define CCORD_MSGFLAG_SUPPRESS_EMBEDS (1u << 2)
#define CCORD_MSGFLAG_SOURCE_DELETED (1u << 3)
#define CCORD_MSGFLAG_URGENT (1u << 4)
#define CCORD_MSGFLAG_HAS_THREAD (1u << 5)
#define CCORD_MSGFLAG_EPHEMERAL (1u << 6)
#define CCORD_MSGFLAG_LOADING (1u << 7)
#define CCORD_MSGFLAG_SUPPRESS_NOTIFICATIONS (1u << 12)
#define CCORD_MSGFLAG_VOICE_MESSAGE (1u << 13)
#define CCORD_MSGFLAG_HAS_SNAPSHOT (1u << 14)
#define CCORD_MSGFLAG_IS_COMPONENTS_V2 (1u << 15)   // `components` is a v2 tree: no content, embeds, stickers or poll

typedef struct ccord_message_rich {
  const ccord_id *mention_users; size_t mention_user_count;
  const ccord_id *mention_roles; size_t mention_role_count;
  const ccord_attachment *attachments; size_t attachment_count;
  const ccord_embed *embeds; size_t embed_count;
  const ccord_msg_sticker *stickers; size_t sticker_count;
  ccord_msg_reference reference;
  ccord_ref_preview ref_preview;
  ccord_forward forward;
  ccord_poll poll;
  ccord_msg_thread thread;
  ccord_id application_id;       // bot/application that owns the message's components (0 when none)
  const ccord_component_row *components; size_t component_row_count;   // v1 action rows
  // Components v2: the top-level nodes (Container, Section, TextDisplay, MediaGallery, File, Separator, ActionRow). A
  // message with CCORD_MSGFLAG_IS_COMPONENTS_V2 carries its components here instead of in `components`. They travel with
  // CCORD_RICH_HAS_COMPONENTS: an update that carries components replaces both.
  const ccord_component *v2_components; size_t v2_component_count;
  uint32_t present;              // CCORD_RICH_HAS_*: what this block says (everything for a whole message)
  uint32_t flags;                // CCORD_MSGFLAG_*
  uint16_t type;                 // Discord message type: 0 default, 19 reply, 7 join, 8 boost, 6 pin ...
  bool text_absent;              // (an update) it did not carry the message text: the store keeps the stored text.
                                 // Never set on a stored block.
  bool mention_everyone;         // @everyone or @here was used
} ccord_message_rich;

// A deep copy in one owned block (every string becomes ""-or-owned). nullptr when `src` is null (err stays clear) or on
// failure (err set). Release with ccord_message_rich_free.
[[nodiscard]] ccord_message_rich *ccord_message_rich_clone(const ccord_message_rich *src, ccord_error *err);
// The result of an update: the parts `upd` says it carries replace the ones in `old`, the others stay. Either may be
// null (no rich data yet / nothing new); the result is a new owned block, or nullptr when both are null.
[[nodiscard]] ccord_message_rich *ccord_message_rich_merge(const ccord_message_rich *old, const ccord_message_rich *upd,
                                                           ccord_error *err);
// Only for blocks returned by clone / merge.
void ccord_message_rich_free(ccord_message_rich *r);
// Caps, text validity and enum ranges. CCORD_OK for null.
[[nodiscard]] ccord_status ccord_message_rich_validate(const ccord_message_rich *r, ccord_error *err);

// Reading helpers that accept a message without a rich block.
[[nodiscard]] uint16_t ccord_message_type(const ccord_message *m);
[[nodiscard]] uint32_t ccord_message_flags(const ccord_message *m);
[[nodiscard]] bool ccord_message_is_ephemeral(const ccord_message *m);
[[nodiscard]] ccord_status ccord_slash_command_validate(const ccord_slash_command *cmd, ccord_error *err);
[[nodiscard]] bool ccord_rich_mentions_user(const ccord_message_rich *r, ccord_id user_id);
[[nodiscard]] bool ccord_rich_mentions_role(const ccord_message_rich *r, ccord_id role_id);
// Whether the message addresses the user directly: their id is in the message's mention list (which includes a reply
// that pings), or the text has their <@id> / <@!id> tag. @everyone and role pings are not direct.
[[nodiscard]] bool ccord_message_mentions_user(const ccord_message *m, ccord_id user_id);
// Components v2. The first node, searched depth first (children, then accessory) and then the v1 rows, whose custom_id is
// `custom_id`; nullptr when none (or when `custom_id` is null or empty). The pointer lives as long as `rich`.
[[nodiscard]] const ccord_component *ccord_rich_find_component(const ccord_message_rich *rich, const char *custom_id);
// The URL to load for a v2 media item: for "attachment://name" the url of the message attachment with that filename (else
// the item's proxy_url), otherwise its url (else its proxy_url). "" when there is none. Never null.
[[nodiscard]] const char *ccord_rich_media_url(const ccord_message_rich *rich, const ccord_media_ref *ref);
// Image / video / audio by MIME type, falling back to the filename extension.
[[nodiscard]] bool ccord_attachment_is_image(const ccord_attachment *a);
[[nodiscard]] bool ccord_attachment_is_video(const ccord_attachment *a);
[[nodiscard]] bool ccord_attachment_is_audio(const ccord_attachment *a);

// System messages (joins, boosts, pins ...): a type that is not an ordinary message, and the words that follow the
// author's name for it ("joined the server."; nullptr when there is no standard wording).
[[nodiscard]] bool ccord_message_type_is_system(uint16_t type);
[[nodiscard]] const char *ccord_message_type_phrase(uint16_t type);

// A single line saying what a message is: its text, or for a message without text what it carries ("Image", "Sticker: wave",
// "Poll: Pizza?" ...). Newlines become spaces; the result is cut to `cap` - 1 bytes. Returns the length.
size_t ccord_message_preview(const ccord_message *m, char *out, size_t cap);
