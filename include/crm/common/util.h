/*
 * Copyright 2004-2020 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */

#ifndef CRM_COMMON_UTIL__H
#  define CRM_COMMON_UTIL__H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \file
 * \brief Utility functions
 * \ingroup core
 */

#  include <sys/types.h>    // gid_t, mode_t, size_t, time_t, uid_t
#  include <stdlib.h>
#  include <stdbool.h>
#  include <stdint.h>       // uint32_t
#  include <limits.h>
#  include <signal.h>
#  include <glib.h>

#  include <libxml/tree.h>

#  include <crm/lrmd.h>
#  include <crm/common/acl.h>
#  include <crm/common/results.h>

#  define ONLINESTATUS  "online"  // Status of an online client
#  define OFFLINESTATUS "offline" // Status of an offline client

// public name/value pair functions (from nvpair.c)
int pcmk_scan_nvpair(const char *input, char **name, char **value);
char *pcmk_format_nvpair(const char *name, const char *value, const char *units);
char *pcmk_format_named_time(const char *name, time_t epoch_time);

/* public Pacemaker Remote functions (from remote.c) */
int crm_default_remote_port(void);

/* public string functions (from strings.c) */
char *crm_itoa_stack(int an_int, char *buf, size_t len);
gboolean crm_is_true(const char *s);
int crm_str_to_boolean(const char *s, int *ret);
long long crm_parse_ll(const char *text, const char *default_text);
int crm_parse_int(const char *text, const char *default_text);
long long crm_get_msec(const char *input);
char * crm_strip_trailing_newline(char *str);
gboolean crm_str_eq(const char *a, const char *b, gboolean use_case);
gboolean safe_str_neq(const char *a, const char *b);
gboolean crm_strcase_equal(gconstpointer a, gconstpointer b);
guint crm_strcase_hash(gconstpointer v);
guint g_str_hash_traditional(gconstpointer v);
char *crm_strdup_printf(char const *format, ...) __attribute__ ((__format__ (__printf__, 1, 2)));
int pcmk__parse_ll_range(const char *srcstring, long long *start, long long *end);

#  define safe_str_eq(a, b) crm_str_eq(a, b, FALSE)
#  define crm_str_hash g_str_hash_traditional

static inline char *
crm_itoa(int an_int)
{
    return crm_strdup_printf("%d", an_int);
}

static inline char *
crm_ftoa(double a_float)
{
    return crm_strdup_printf("%f", a_float);
}

static inline char *
crm_ttoa(time_t epoch_time)
{
    return crm_strdup_printf("%lld", (long long) epoch_time);
}

/*!
 * \brief Create hash table with dynamically allocated string keys/values
 *
 * \return Newly allocated hash table
 * \note It is the caller's responsibility to free the result, using
 *       g_hash_table_destroy().
 */
static inline GHashTable *
crm_str_table_new()
{
    return g_hash_table_new_full(crm_str_hash, g_str_equal, free, free);
}

/*!
 * \brief Create hash table with case-insensitive dynamically allocated string keys/values
 *
 * \return Newly allocated hash table
 * \note It is the caller's responsibility to free the result, using
 *       g_hash_table_destroy().
 */
static inline GHashTable *
crm_strcase_table_new()
{
    return g_hash_table_new_full(crm_strcase_hash, crm_strcase_equal, free, free);
}

GHashTable *crm_str_table_dup(GHashTable *old_table);

#  define crm_atoi(text, default_text) crm_parse_int(text, default_text)

/* public I/O functions (from io.c) */
void crm_build_path(const char *path_c, mode_t mode);

guint crm_parse_interval_spec(const char *input);
int char2score(const char *score);
char *score2char(int score);
char *score2char_stack(int score, char *buf, size_t len);

/* public operation functions (from operations.c) */
gboolean parse_op_key(const char *key, char **rsc_id, char **op_type,
                      guint *interval_ms);
gboolean decode_transition_key(const char *key, char **uuid, int *transition_id,
                               int *action_id, int *target_rc);
gboolean decode_transition_magic(const char *magic, char **uuid,
                                 int *transition_id, int *action_id,
                                 int *op_status, int *op_rc, int *target_rc);
int rsc_op_expected_rc(lrmd_event_data_t *event);
gboolean did_rsc_op_fail(lrmd_event_data_t *event, int target_rc);
bool crm_op_needs_metadata(const char *rsc_class, const char *op);
xmlNode *crm_create_op_xml(xmlNode *parent, const char *prefix,
                           const char *task, const char *interval_spec,
                           const char *timeout);
#define CRM_DEFAULT_OP_TIMEOUT_S "20s"

// Public resource agent functions (from agents.c)

// Capabilities supported by a resource agent standard
enum pcmk_ra_caps {
    pcmk_ra_cap_none         = 0,
    pcmk_ra_cap_provider     = (1 << 0), // Requires provider
    pcmk_ra_cap_status       = (1 << 1), // Supports status instead of monitor
    pcmk_ra_cap_params       = (1 << 2), // Supports parameters
    pcmk_ra_cap_unique       = (1 << 3), // Supports unique clones
    pcmk_ra_cap_promotable   = (1 << 4), // Supports promotable clones
    pcmk_ra_cap_stdin        = (1 << 5), // Reads from standard input
};

uint32_t pcmk_get_ra_caps(const char *standard);
char *crm_generate_ra_key(const char *standard, const char *provider,
                          const char *type);
int crm_parse_agent_spec(const char *spec, char **standard, char **provider,
                         char **type);


int compare_version(const char *version1, const char *version2);

/* coverity[+kill] */
void crm_abort(const char *file, const char *function, int line,
               const char *condition, gboolean do_core, gboolean do_fork);

static inline gboolean
is_not_set(long long word, long long bit)
{
    return ((word & bit) == 0);
}

static inline gboolean
is_set(long long word, long long bit)
{
    return ((word & bit) == bit);
}

static inline gboolean
is_set_any(long long word, long long bit)
{
    return ((word & bit) != 0);
}

static inline guint
crm_hash_table_size(GHashTable * hashtable)
{
    if (hashtable == NULL) {
        return 0;
    }
    return g_hash_table_size(hashtable);
}

char *crm_meta_name(const char *field);
const char *crm_meta_value(GHashTable * hash, const char *field);

char *crm_md5sum(const char *buffer);

char *crm_generate_uuid(void);
bool crm_is_daemon_name(const char *name);

int crm_user_lookup(const char *name, uid_t * uid, gid_t * gid);
int pcmk_daemon_user(uid_t *uid, gid_t *gid);

#ifdef HAVE_GNUTLS_GNUTLS_H
void crm_gnutls_global_init(void);
#endif

char *pcmk_hostname(void);

bool pcmk_str_is_infinity(const char *s);
bool pcmk_str_is_minus_infinity(const char *s);

#ifndef PCMK__NO_COMPAT
/* Everything here is deprecated and kept only for public API backward
 * compatibility. It will be moved to compatibility.h when 2.1.0 is released.
 */

//! \deprecated Use crm_parse_interval_spec() instead
#define crm_get_interval crm_parse_interval_spec

//! \deprecated Use pcmk_get_ra_caps() instead
bool crm_provider_required(const char *standard);

#endif // PCMK__NO_COMPAT

#ifdef __cplusplus
}
#endif

#endif
