/*
     This file is part of libmicrohttpd
     Copyright (C) 2010, 2011, 2012, 2015, 2018 Daniel Pittman and Christian Grothoff
     Copyright (C) 2014-2022 Evgeny Grin (Karlson2k)

     This library is free software; you can redistribute it and/or
     modify it under the terms of the GNU Lesser General Public
     License as published by the Free Software Foundation; either
     version 2.1 of the License, or (at your option) any later version.

     This library is distributed in the hope that it will be useful,
     but WITHOUT ANY WARRANTY; without even the implied warranty of
     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
     Lesser General Public License for more details.

     You should have received a copy of the GNU Lesser General Public
     License along with this library; if not, write to the Free Software
     Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/
/**
 * @file digestauth.c
 * @brief Implements HTTP digest authentication
 * @author Amr Ali
 * @author Matthieu Speder
 * @author Christian Grothoff (RFC 7616 support)
 * @author Karlson2k (Evgeny Grin)
 */
#include "digestauth.h"
#include "gen_auth.h"
#include "platform.h"
#include "mhd_limits.h"
#include "internal.h"
#include "md5.h"
#include "sha256.h"
#include "mhd_mono_clock.h"
#include "mhd_str.h"
#include "mhd_compat.h"
#include "mhd_bithelpers.h"
#include "mhd_assert.h"

#if defined(MHD_W32_MUTEX_)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif /* !WIN32_LEAN_AND_MEAN */
#include <windows.h>
#endif /* MHD_W32_MUTEX_ */


/**
 * Allow re-use of the nonce-nc map array slot after #REUSE_TIMEOUT seconds,
 * if this slot is needed for the new nonce, while the old nonce was not used
 * even one time by the client.
 * Typically clients immediately use generated nonce for new request.
 */
#define REUSE_TIMEOUT 30

/**
 * The maximum value of artificial timestamp difference to avoid clashes.
 * The value must be suitable for bitwise AND operation.
 */
#define DAUTH_JUMPBACK_MAX (0x7F)


/**
 * 48 bit value in bytes
 */
#define TIMESTAMP_BIN_SIZE (48 / 8)


/**
 * Trim value to the TIMESTAMP_BIN_SIZE size
 */
#define TRIM_TO_TIMESTAMP(value) \
  ((value) & ((UINT64_C(1) << (TIMESTAMP_BIN_SIZE * 8)) - 1))


/**
 * The printed timestamp size in chars
 */
#define TIMESTAMP_CHARS_LEN (TIMESTAMP_BIN_SIZE * 2)


/**
 * Standard server nonce length, not including terminating null,
 *
 * @param digest_size digest size
 */
#define NONCE_STD_LEN(digest_size) \
  ((digest_size) * 2 + TIMESTAMP_CHARS_LEN)


/**
 * Maximum size of any digest hash supported by MHD.
 * (SHA-256 > MD5).
 */
#define MAX_DIGEST SHA256_DIGEST_SIZE

/**
 * Macro to avoid using VLAs if the compiler does not support them.
 */
#ifndef HAVE_C_VARARRAYS
/**
 * Return #MAX_DIGEST.
 *
 * @param n length of the digest to be used for a VLA
 */
#define VLA_ARRAY_LEN_DIGEST(n) (MAX_DIGEST)

#else
/**
 * Return @a n.
 *
 * @param n length of the digest to be used for a VLA
 */
#define VLA_ARRAY_LEN_DIGEST(n) (n)
#endif

/**
 * Check that @a n is below #MAX_DIGEST
 */
#define VLA_CHECK_LEN_DIGEST(n) \
  do { if ((n) > MAX_DIGEST) MHD_PANIC (_ ("VLA too big.\n")); } while (0)

/**
 * Maximum length of a username for digest authentication.
 */
#define MAX_USERNAME_LENGTH 128

/**
 * Maximum length of a realm for digest authentication.
 */
#define MAX_REALM_LENGTH 256

/**
 * Maximum length of the response in digest authentication.
 */
#define MAX_AUTH_RESPONSE_LENGTH 256

/**
 * The token for MD5 algorithm.
 */
#define _MHD_MD5_TOKEN "MD5"

/**
 * The token for SHA-256 algorithm.
 */
#define _MHD_SHA256_TOKEN "SHA-256"

/**
 * The postfix token for "session" algorithms.
 */
#define _MHD_SESS_TOKEN "-sess"

/**
 * The result of nonce-nc map array check.
 */
enum MHD_CheckNonceNC_
{
  /**
   * The nonce and NC are OK (valid and NC was not used before).
   */
  MHD_CHECK_NONCENC_OK = MHD_DAUTH_OK,

  /**
   * The 'nonce' was overwritten with newer 'nonce' in the same slot or
   * NC was already used.
   * The validity of the 'nonce' was not be checked.
   */
  MHD_CHECK_NONCENC_STALE = MHD_DAUTH_NONCE_STALE,

  /**
   * The 'nonce' is wrong, it was not generated before.
   */
  MHD_CHECK_NONCENC_WRONG = MHD_DAUTH_NONCE_WRONG,
};


/**
 * Digest context data
 */
union DigestCtx
{
  struct MD5Context md5_ctx;
  struct Sha256Ctx sha256_ctx;
};

/**
 * Digest printed as hex digits.
 */
union DigestHex
{
  char md5[MD5_DIGEST_STRING_LENGTH];
  char sha256[SHA256_DIGEST_STRING_SIZE];
};

/**
 * Digest in binary form.
 */
union DigestBin
{
  uint8_t md5[MD5_DIGEST_SIZE];
  uint8_t sha256[SHA256_DIGEST_SIZE];
};

/**
 * The digest calculation structure.
 */
struct DigestAlgorithm
{
  /**
   * A context for the digest algorithm, already initialized to be
   * useful for @e init, @e update and @e digest.
   */
  union DigestCtx ctx;

  /**
   * Digest in binary form.
   */
  union DigestBin digest;
  /**
   * The digest algorithm.
   */
  enum MHD_DigestAuthAlgorithm algo;

  /**
   * Buffer for hex-print of the final digest.
   */
  union DigestHex digest_hex;
#if _DEBUG
  bool setup; /**< The structure was set-up */
  bool inited; /**< The calculation was initialised */
  bool digest_calculated; /**< The digest was calculated */
#endif /* _DEBUG */
};


/**
 * Return name of the algorithm as a string.
 * @param da the digest calculation structure to identify
 * @return the name of the @a algo as a string.
 */
_MHD_static_inline const char *
digest_get_algo_name (struct DigestAlgorithm *da)
{
  mhd_assert (da->setup);
  if (MHD_DIGEST_ALG_MD5 == da->algo)
    return _MHD_MD5_TOKEN;
  if (MHD_DIGEST_ALG_SHA256 == da->algo)
    return _MHD_SHA256_TOKEN;
  mhd_assert (0); /* May not happen */
  return "";
}


/**
 * Return the size of the digest.
 * @param da the digest calculation structure to identify
 * @return the size of the digest.
 */
_MHD_static_inline unsigned int
digest_get_size (struct DigestAlgorithm *da)
{
  mhd_assert (da->setup);
  if (MHD_DIGEST_ALG_MD5 == da->algo)
    return MD5_DIGEST_SIZE;
  if (MHD_DIGEST_ALG_SHA256 == da->algo)
    return SHA256_DIGEST_SIZE;
  mhd_assert (0); /* May not happen */
  return 0;
}


/**
 * Set-up the digest calculation structure.
 * @param da the structure to set-up
 * @param algo the algorithm to use for digest calculation
 * @return boolean 'true' if successfully set-up,
 *         false otherwise.
 */
_MHD_static_inline bool
digest_setup (struct DigestAlgorithm *da,
              enum MHD_DigestAuthAlgorithm algo)
{
#ifdef _DEBUG
  da->setup = false;
  da->inited = false;
  da->digest_calculated = false;
#endif /* _DEBUG */
  if (MHD_DIGEST_ALG_AUTO == algo)
    algo = MHD_DIGEST_ALG_SHA256;

  if ((MHD_DIGEST_ALG_MD5 == algo) ||
      (MHD_DIGEST_ALG_SHA256 == algo))
  {
    da->algo = algo;
#ifdef _DEBUG
    da->setup = true;
#endif /* _DEBUG */
    return true;
  }
  mhd_assert (0); /* Bad parameter */
  return false;
}


/**
 * Initialise/reset the digest calculation structure.
 * @param da the structure to initialise/reset
 */
_MHD_static_inline void
digest_init (struct DigestAlgorithm *da)
{
  mhd_assert (da->setup);
#ifdef _DEBUG
  da->digest_calculated = false;
#endif
  if (MHD_DIGEST_ALG_MD5 == da->algo)
  {
    MHD_MD5Init (&da->ctx.md5_ctx);
#ifdef _DEBUG
    da->inited = true;
#endif
  }
  else if (MHD_DIGEST_ALG_SHA256 == da->algo)
  {
    MHD_SHA256_init (&da->ctx.sha256_ctx);
#ifdef _DEBUG
    da->inited = true;
#endif
  }
  else
  {
#ifdef _DEBUG
    da->inited = false;
#endif
    mhd_assert (0); /* Bad algorithm */
  }
}


/**
 * Feed digest calculation with more data.
 * @param da the digest calculation
 * @param data the data to process
 * @param length the size of the @a data in bytes
 */
_MHD_static_inline void
digest_update (struct DigestAlgorithm *da,
               const uint8_t *data,
               size_t length)
{
  mhd_assert (da->inited);
  mhd_assert (! da->digest_calculated);
  if (MHD_DIGEST_ALG_MD5 == da->algo)
    MHD_MD5Update (&da->ctx.md5_ctx, data, length);
  else if (MHD_DIGEST_ALG_SHA256 == da->algo)
    MHD_SHA256_update (&da->ctx.sha256_ctx, data, length);
  else
    mhd_assert (0); /* May not happen */
}


/**
 * Finally calculate hash (the digest).
 * @param da the digest calculation
 */
_MHD_static_inline void
digest_calc_hash (struct DigestAlgorithm *da)
{
  mhd_assert (da->inited);
  mhd_assert (! da->digest_calculated);
  if (MHD_DIGEST_ALG_MD5 == da->algo)
    MHD_MD5Final (&da->ctx.md5_ctx, da->digest.md5);
  else if (MHD_DIGEST_ALG_SHA256 == da->algo)
    MHD_SHA256_finish (&da->ctx.sha256_ctx, da->digest.sha256);
  else
    mhd_assert (0); /* May not happen */
#ifdef _DEBUG
  da->digest_calculated = true;
#endif
}


/**
 * Get pointer to the calculated digest in binary form.
 * @param da the digest calculation
 * @return the pointer to the calculated digest
 */
_MHD_static_inline const uint8_t *
digest_get_bin (struct DigestAlgorithm *da)
{
  mhd_assert (da->inited);
  mhd_assert (da->digest_calculated);
  mhd_assert (da->digest.md5 == da->digest.sha256);
  return da->digest.sha256;
}


/**
 * Get pointer to the buffer for the printed digest.
 * @param da the digest calculation
 * @return the pointer to the buffer
 */
_MHD_static_inline char *
digest_get_hex_buffer (struct DigestAlgorithm *da)
{
  return da->digest_hex.sha256;
}


/**
 * Put calculated digest to the buffer as hex digits.
 * @param da the digest calculation
 * @return the pointer to the calculated digest
 */
_MHD_static_inline void
digest_make_hex (struct DigestAlgorithm *da)
{
  MHD_bin_to_hex (digest_get_bin (da),
                  digest_get_size (da),
                  digest_get_hex_buffer (da));
}


/**
 * calculate H(A1) from given hash as per RFC2617 spec
 * and store the result in 'digest_hex'.
 *
 * @param alg The hash algorithm used, can be "MD5" or "MD5-sess"
 *            or "SHA-256" or "SHA-256-sess"
 *    Note that the rest of the code does not support the the "-sess" variants!
 * @param[in,out] da digest implementation, must match @a alg; the
 *          da->digest_hex will be initialized to the digest in HEX
 * @param digest An `unsigned char *' pointer to the binary MD5 sum
 *      for the precalculated hash value "username:realm:password"
 *      of #MHD_MD5_DIGEST_SIZE or #SHA256_DIGEST_SIZE bytes
 * @param nonce A `char *' pointer to the nonce value
 * @param cnonce A `char *' pointer to the cnonce value
 */
static void
digest_calc_ha1_from_digest (const char *alg,
                             struct DigestAlgorithm *da,
                             const uint8_t *digest,
                             const char *nonce,
                             const char *cnonce)
{
  /* TODO: disable unsupported code paths */
  if ( (MHD_str_equal_caseless_ (alg,
                                 _MHD_MD5_TOKEN _MHD_SESS_TOKEN)) ||
       (MHD_str_equal_caseless_ (alg,
                                 _MHD_SHA256_TOKEN _MHD_SESS_TOKEN)) )
  {
    digest_init (da);
    digest_update (da,
                   digest,
                   digest_get_size (da));
    digest_update (da,
                   (const unsigned char *) ":",
                   1);
    digest_update (da,
                   (const unsigned char *) nonce,
                   strlen (nonce));
    digest_update (da,
                   (const unsigned char *) ":",
                   1);
    digest_update (da,
                   (const unsigned char *) cnonce,
                   strlen (cnonce));
    digest_calc_hash (da);
    digest_make_hex (da);
  }
  else
  {
    MHD_bin_to_hex (digest,
                    digest_get_size (da),
                    digest_get_hex_buffer (da));
  }
}


/**
 * calculate H(A1) from username, realm and password as per RFC2617 spec
 * and store the result in 'digest_hex'.
 *
 * @param alg The hash algorithm used, can be "MD5" or "MD5-sess"
 *             or "SHA-256" or "SHA-256-sess"
 * @param username A `char *' pointer to the username value
 * @param username the length of the @a username
 * @param realm A `char *' pointer to the realm value
 * @param realm_len the length of the @a realm
 * @param password A `char *' pointer to the password value
 * @param nonce A `char *' pointer to the nonce value
 * @param cnonce A `char *' pointer to the cnonce value
 * @param[in,out] da digest algorithm to use, and where to write
 *         the sessionkey to
 */
static void
digest_calc_ha1_from_user (const char *alg,
                           const char *username,
                           size_t username_len,
                           const char *realm,
                           size_t realm_len,
                           const char *password,
                           const char *nonce,
                           const char *cnonce,
                           struct DigestAlgorithm *da)
{
  digest_init (da);
  digest_update (da,
                 (const unsigned char *) username,
                 username_len);
  digest_update (da,
                 (const unsigned char *) ":",
                 1);
  digest_update (da,
                 (const unsigned char *) realm,
                 realm_len);
  digest_update (da,
                 (const unsigned char *) ":",
                 1);
  digest_update (da,
                 (const unsigned char *) password,
                 strlen (password));
  digest_calc_hash (da);
  digest_calc_ha1_from_digest (alg,
                               da,
                               digest_get_bin (da),
                               nonce,
                               cnonce);
}


/**
 * Calculate request-digest/response-digest as per RFC2617 / RFC7616
 * spec.
 *
 * @param ha1 H(A1), twice the @a da->digest_size + 1 bytes (0-terminated),
 *        MUST NOT be aliased with `da->sessionkey`!
 * @param nonce nonce from server
 * @param noncecount 8 hex digits
 * @param cnonce client nonce
 * @param qop qop-value: "", "auth" or "auth-int" (NOTE: only 'auth' is supported today.)
 * @param method method from request
 * @param uri requested URL, could be not zero-terminated
 * @param uri_len the length of @a uri, in characters
 * @param hentity H(entity body) if qop="auth-int"
 * @param[in,out] da digest algorithm to use, also
 *        we write da->sessionkey (set to response request-digest or response-digest)
 */
static void
digest_calc_response (const char *ha1,
                      const char *nonce,
                      const char *noncecount,
                      const char *cnonce,
                      const char *qop,
                      const char *method,
                      const char *uri,
                      size_t uri_len,
                      const char *hentity,
                      struct DigestAlgorithm *da)
{
  (void) hentity; /* Unused. Silence compiler warning. */

  /* Calculate h(a2) */
  digest_init (da);
  digest_update (da,
                 (const unsigned char *) method,
                 strlen (method));
  digest_update (da,
                 (const unsigned char *) ":",
                 1);
  digest_update (da,
                 (const unsigned char *) uri,
                 uri_len);
#if 0
  if (0 == strcasecmp (qop,
                       "auth-int"))
  {
    /* This is dead code since the rest of this module does
 not support auth-int. */
    digest_update (da,
                   ":",
                   1);
    if (NULL != hentity)
      da->update (da->ctx,
                  hentity,
                  strlen (hentity));
  }
#endif
  digest_calc_hash (da);
  digest_make_hex (da);

  /* calculate response */
  digest_init (da);
  digest_update (da,
                 (const unsigned char *) ha1,
                 digest_get_size (da) * 2);
  digest_update (da,
                 (const unsigned char *) ":",
                 1);
  digest_update (da,
                 (const unsigned char *) nonce,
                 strlen (nonce));
  digest_update (da,
                 (const unsigned char *) ":",
                 1);
  if ('\0' != *qop)
  {
    digest_update (da,
                   (const unsigned char *) noncecount,
                   strlen (noncecount));
    digest_update (da,
                   (const unsigned char *) ":",
                   1);
    digest_update (da,
                   (const unsigned char *) cnonce,
                   strlen (cnonce));
    digest_update (da,
                   (const unsigned char *) ":",
                   1);
    digest_update (da,
                   (const unsigned char *) qop,
                   strlen (qop));
    digest_update (da,
                   (const unsigned char *) ":",
                   1);
  }
  digest_update (da,
                 (const unsigned char *) digest_get_hex_buffer (da),
                 digest_get_size (da) * 2);

  digest_calc_hash (da);
  digest_make_hex (da);
}


static const struct MHD_RqDAuth *
get_rq_dauth_params (struct MHD_Connection *connection)
{
  const struct MHD_AuthRqHeader *rq_params;

  rq_params = MHD_get_auth_rq_params_ (connection);
  if ( (NULL == rq_params) ||
       (MHD_AUTHTYPE_DIGEST != rq_params->auth_type) )
    return NULL;

  return rq_params->params.dauth;
}


/**
 * Extract timestamp from the given nonce.
 * @param nonce the nonce to check
 * @param noncelen the lenght of the nonce, zero for autodetect
 * @param[out] ptimestamp the pointer to store extracted timestamp
 * @return true if timestamp was extracted,
 *         false if nonce does not have valid timestamp.
 */
static bool
get_nonce_timestamp (const char *const nonce,
                     size_t noncelen,
                     uint64_t *const ptimestamp)
{
  if (0 == noncelen)
    noncelen = strlen (nonce);

  if ( (NONCE_STD_LEN (SHA256_DIGEST_SIZE) != noncelen) &&
       (NONCE_STD_LEN (MD5_DIGEST_SIZE) != noncelen) )
    return false;

  if (TIMESTAMP_CHARS_LEN !=
      MHD_strx_to_uint64_n_ (nonce + noncelen - TIMESTAMP_CHARS_LEN,
                             TIMESTAMP_CHARS_LEN,
                             ptimestamp))
    return false;
  return true;
}


/**
 * Super-fast xor-based "hash" function
 *
 * @param data the data to calculate hash for
 * @param data_size the size of the data in bytes
 * @return the "hash"
 */
static uint32_t
fast_simple_hash (const uint8_t *data,
                  size_t data_size)
{
  uint32_t hash;

  if (0 != data_size)
  {
    size_t i;
    hash = data[0];
    for (i = 1; i < data_size; i++)
      hash = _MHD_ROTL32 (hash, 7) ^ data[i];
  }
  else
    hash = 0;

  return hash;
}


/**
 * Get index of the nonce in the nonce-nc map array.
 *
 * @param arr_size the size of nonce_nc array
 * @param nonce the pointer that referenced a zero-terminated array of nonce
 * @param noncelen the lenth of @a nonce, in characters
 * @return #MHD_YES if successful, #MHD_NO if invalid (or we have no NC array)
 */
static size_t
get_nonce_nc_idx (size_t arr_size,
                  const char *nonce,
                  size_t noncelen)
{
  mhd_assert (0 != arr_size);
  mhd_assert (0 != noncelen);
  return fast_simple_hash ((const uint8_t *) nonce, noncelen) % arr_size;
}


/**
 * Check nonce-nc map array with the new nonce counter.
 *
 * @param connection The MHD connection structure
 * @param nonce A pointer that referenced a zero-terminated array of nonce
 * @param noncelen the length of @a nonce, in characters
 * @param nc The nonce counter
 * @return #MHD_DAUTH_NONCENC_OK if successful,
 *         #MHD_DAUTH_NONCENC_STALE if nonce is stale (or no nonce-nc array
 *         is available),
 *         #MHD_DAUTH_NONCENC_WRONG if nonce was not recodered in nonce-nc map
 *         array, while it should.
 */
static enum MHD_CheckNonceNC_
check_nonce_nc (struct MHD_Connection *connection,
                const char *nonce,
                size_t noncelen,
                uint64_t nonce_time,
                uint64_t nc)
{
  struct MHD_Daemon *daemon = MHD_get_master (connection->daemon);
  struct MHD_NonceNc *nn;
  uint32_t mod;
  enum MHD_CheckNonceNC_ ret;

  mhd_assert (0 != noncelen);
  mhd_assert (strlen (nonce) == noncelen);
  mhd_assert (0 != nc);
  if (MAX_DIGEST_NONCE_LENGTH < noncelen)
    return MHD_CHECK_NONCENC_WRONG; /* This should be impossible, but static analysis
                      tools have a hard time with it *and* this also
                      protects against unsafe modifications that may
                      happen in the future... */
  mod = daemon->nonce_nc_size;
  if (0 == mod)
    return MHD_CHECK_NONCENC_STALE;  /* no array! */
  if (nc >= UINT64_MAX - 64)
    return MHD_CHECK_NONCENC_STALE;  /* Overflow, unrealistically high value */

  nn = &daemon->nnc[get_nonce_nc_idx (mod, nonce, noncelen)];

  MHD_mutex_lock_chk_ (&daemon->nnc_lock);

  mhd_assert (0 == nn->nonce[noncelen]); /* The old value must be valid */

  if ( (0 != memcmp (nn->nonce, nonce, noncelen)) ||
       (0 != nn->nonce[noncelen]) )
  { /* The nonce in the slot does not match nonce from the client */
    if (0 == nn->nonce[0])
    { /* The slot was never used, while the client's nonce value should be
       * recorded when it was generated by MHD */
      ret = MHD_CHECK_NONCENC_WRONG;
    }
    else if (0 != nn->nonce[noncelen])
    { /* The value is the slot is wrong */
      ret =  MHD_CHECK_NONCENC_STALE;
    }
    else
    {
      uint64_t slot_ts; /**< The timestamp in the slot */
      if (! get_nonce_timestamp (nn->nonce, 0, &slot_ts))
      {
        mhd_assert (0); /* The value is the slot is wrong */
        ret = MHD_CHECK_NONCENC_STALE;
      }
      else
      {
        /* Unsigned value, will be large if nonce_time is less than slot_ts */
        const uint64_t ts_diff = TRIM_TO_TIMESTAMP (nonce_time - slot_ts);
        if ((REUSE_TIMEOUT * 1000) >= ts_diff)
        {
          /* The nonce from the client may not have been placed in the slot
           * because another nonce in that slot has not yet expired. */
          ret = MHD_CHECK_NONCENC_STALE;
        }
        else if (TRIM_TO_TIMESTAMP (UINT64_MAX) / 2 >= ts_diff)
        {
          /* Too large value means that nonce_time is less than slot_ts.
           * The nonce from the client may have been overwritten by the newer
           * nonce. */
          ret = MHD_CHECK_NONCENC_STALE;
        }
        else
        {
          /* The nonce from the client should be generated after the nonce
           * in the slot has been expired, the nonce must be recorded, but
           * it's not. */
          ret = MHD_CHECK_NONCENC_WRONG;
        }
      }
    }
  }
  else if (nc > nn->nc)
  {
    /* 'nc' is larger, shift bitmask and bump limit */
    const uint64_t jump_size = nc - nn->nc;
    if (64 > jump_size)
    {
      /* small jump, less than mask width */
      nn->nmask <<= jump_size;
      /* Set bit for the old 'nc' value */
      nn->nmask |= (UINT64_C (1) << (jump_size - 1));
    }
    else if (64 == jump_size)
      nn->nmask = (UINT64_C (1) << 63);
    else
      nn->nmask = 0;                /* big jump, unset all bits in the mask */
    nn->nc = nc;
    ret = MHD_CHECK_NONCENC_OK;
  }
  else if (nc < nn->nc)
  {
    /* Note that we use 64 here, as we do not store the
       bit for 'nn->nc' itself in 'nn->nmask' */
    if ( (nc + 64 >= nn->nc) &&
         (0 == ((UINT64_C (1) << (nn->nc - nc - 1)) & nn->nmask)) )
    {
      /* Out-of-order nonce, but within 64-bit bitmask, set bit */
      nn->nmask |= (UINT64_C (1) << (nn->nc - nc - 1));
      ret = MHD_CHECK_NONCENC_OK;
    }
    else
      /* 'nc' was already used or too old (more then 64 values ago) */
      ret = MHD_CHECK_NONCENC_STALE;
  }
  else /* if (nc == nn->nc) */
    /* 'nc' was already used */
    ret = MHD_CHECK_NONCENC_STALE;

  MHD_mutex_unlock_chk_ (&daemon->nnc_lock);

  return ret;
}


/**
 * Get the username from the authorization header sent by the client
 *
 * @param connection The MHD connection structure
 * @return NULL if no username could be found, a pointer
 *      to the username if found
 * @warning Returned value must be freed by #MHD_free().
 * @ingroup authentication
 */
_MHD_EXTERN char *
MHD_digest_auth_get_username (struct MHD_Connection *connection)
{
  const struct MHD_RqDAuth *params;
  char *username;
  size_t username_len;

  params = get_rq_dauth_params (connection);
  if (NULL == params)
    return NULL;

  if (NULL == params->username.value.str)
    return NULL;

  username_len = params->username.value.len;
  username = malloc (username_len + 1);
  if (NULL == username)
    return NULL;

  if (! params->username.quoted)
  {
    /* The username is not quoted, no need to unquote */
    if (0 != username_len)
      memcpy (username, params->username.value.str, username_len);
    username[username_len] = 0; /* Zero-terminate */
  }
  else
  {
    /* Need to properly unquote the username */
    mhd_assert (0 != username_len); /* Quoted string may not be zero-legth */
    username_len = MHD_str_unquote (params->username.value.str, username_len,
                                    username);
    mhd_assert (0 != username_len); /* The unquoted string cannot be empty */
    username[username_len] = 0; /* Zero-terminate */
  }

  return username;
}


/**
 * Calculate the server nonce so that it mitigates replay attacks
 * The current format of the nonce is ...
 * H(timestamp ":" method ":" random ":" uri ":" realm) + Hex(timestamp)
 *
 * @param nonce_time The amount of time in seconds for a nonce to be invalid
 * @param method HTTP method
 * @param rnd A pointer to a character array for the random seed
 * @param rnd_size The size of the random seed array @a rnd
 * @param uri HTTP URI (in MHD, without the arguments ("?k=v")
 * @param realm A string of characters that describes the realm of auth.
 * @param realm_len the length of the @a realm.
 * @param da digest algorithm to use
 * @param[out] nonce A pointer to a character array for the nonce to put in,
 *        must provide NONCE_STD_LEN(da->digest_size)+1 bytes
 */
static void
calculate_nonce (uint64_t nonce_time,
                 const char *method,
                 const char *rnd,
                 size_t rnd_size,
                 const char *uri,
                 const char *realm,
                 size_t realm_len,
                 struct DigestAlgorithm *da,
                 char *nonce)
{
  uint8_t timestamp[TIMESTAMP_BIN_SIZE];

  digest_init (da);
  /* If the nonce_time is milliseconds, then the same 48 bit value will repeat
   * every 8 925 years, which is more than enough to mitigate a replay attack */
#if TIMESTAMP_BIN_SIZE != 6
#error The code needs to be updated here
#endif
  timestamp[0] = (uint8_t) (nonce_time >> (8 * (TIMESTAMP_BIN_SIZE - 1 - 0)));
  timestamp[1] = (uint8_t) (nonce_time >> (8 * (TIMESTAMP_BIN_SIZE - 1 - 1)));
  timestamp[2] = (uint8_t) (nonce_time >> (8 * (TIMESTAMP_BIN_SIZE - 1 - 2)));
  timestamp[3] = (uint8_t) (nonce_time >> (8 * (TIMESTAMP_BIN_SIZE - 1 - 3)));
  timestamp[4] = (uint8_t) (nonce_time >> (8 * (TIMESTAMP_BIN_SIZE - 1 - 4)));
  timestamp[5] = (uint8_t) (nonce_time >> (8 * (TIMESTAMP_BIN_SIZE - 1 - 5)));
  digest_update (da,
                 timestamp,
                 sizeof (timestamp));
  digest_update (da,
                 (const unsigned char *) ":",
                 1);
  digest_update (da,
                 (const unsigned char *) method,
                 strlen (method));
  digest_update (da,
                 (const unsigned char *) ":",
                 1);
  if (rnd_size > 0)
    digest_update (da,
                   (const unsigned char *) rnd,
                   rnd_size);
  digest_update (da,
                 (const unsigned char *) ":",
                 1);
  digest_update (da,
                 (const unsigned char *) uri,
                 strlen (uri));
  digest_update (da,
                 (const unsigned char *) ":",
                 1);
  digest_update (da,
                 (const unsigned char *) realm,
                 realm_len);
  digest_calc_hash (da);
  MHD_bin_to_hex (digest_get_bin (da),
                  digest_get_size (da),
                  nonce);
  MHD_bin_to_hex (timestamp,
                  sizeof (timestamp),
                  nonce + digest_get_size (da) * 2);
}


/**
 * Check whether it is possible to use slot in nonce-nc map array.
 *
 * Should be called with mutex held to avoid external modification of
 * the slot data.
 *
 * @param nn the pointer to the nonce-nc slot
 * @param now the current time
 * @param new_nonce the new nonce supposed to be stored in this slot,
 *                  zero-terminated
 * @param new_nonce_len the length of the @a new_nonce in chars, not including
 *                      the terminating zero.
 * @return true if the slot can be used to store the new nonce,
 *         false otherwise.
 */
static bool
is_slot_available (const struct MHD_NonceNc *const nn,
                   const uint64_t now,
                   const char *const new_nonce,
                   size_t new_nonce_len)
{
  uint64_t timestamp;
  bool timestamp_valid;
  mhd_assert (new_nonce_len <= NONCE_STD_LEN (MAX_DIGEST));
  mhd_assert (NONCE_STD_LEN (MAX_DIGEST) <= MAX_DIGEST_NONCE_LENGTH);
  if (0 == nn->nonce[0])
    return true; /* The slot is empty */

  if ((0 == memcmp (nn->nonce, new_nonce, new_nonce_len)) &&
      (0 == nn->nonce[new_nonce_len]))
  {
    /* The slot has the same nonce already, the same nonce was already generated
     * and used, this slot cannot be used with the same nonce as it would
     * just reset received 'nc' values. */
    return false;
  }

  if (0 != nn->nc)
    return true; /* Client already used the nonce in this slot at least
                    one time, re-use the slot */

  /* The nonce must be zero-terminated */
  mhd_assert (0 == nn->nonce[sizeof(nn->nonce) - 1]);
  if (0 != nn->nonce[sizeof(nn->nonce) - 1])
    return true; /* Wrong nonce format in the slot */

  timestamp_valid = get_nonce_timestamp (nn->nonce, 0, &timestamp);
  mhd_assert (timestamp_valid);
  if (! timestamp_valid)
    return true; /* Invalid timestamp in nonce-nc, should not be possible */

  if ((REUSE_TIMEOUT * 1000) < TRIM_TO_TIMESTAMP (now - timestamp))
    return true;

  return false;
}


/**
 * Calculate the server nonce so that it mitigates replay attacks and add
 * the new nonce to the nonce-nc map array.
 *
 * @param connection the MHD connection structure
 * @param timestamp the current timestamp
 * @param realm the string of characters that describes the realm of auth
 * @param realm_len the lenght of the @a realm
 * @param da the digest algorithm to use
 * @param[out] nonce the pointer to a character array for the nonce to put in,
 *        must provide NONCE_STD_LEN(da->digest_size)+1 bytes
 * @return true if the new nonce has been added to the nonce-nc map array,
 *         false otherwise.
 */
static bool
calculate_add_nonce (struct MHD_Connection *const connection,
                     uint64_t timestamp,
                     const char *realm,
                     size_t realm_len,
                     struct DigestAlgorithm *da,
                     char *nonce)
{
  struct MHD_Daemon *const daemon = MHD_get_master (connection->daemon);
  struct MHD_NonceNc *nn;
  const size_t nonce_size = NONCE_STD_LEN (digest_get_size (da));
  bool ret;

  mhd_assert (MAX_DIGEST_NONCE_LENGTH >= nonce_size);
  mhd_assert (0 != nonce_size);

  calculate_nonce (timestamp,
                   connection->method,
                   daemon->digest_auth_random,
                   daemon->digest_auth_rand_size,
                   connection->url,
                   realm,
                   realm_len,
                   da,
                   nonce);

  if (0 == daemon->nonce_nc_size)
    return false;

  nn = daemon->nnc + get_nonce_nc_idx (daemon->nonce_nc_size,
                                       nonce,
                                       nonce_size);

  MHD_mutex_lock_chk_ (&daemon->nnc_lock);
  if (is_slot_available (nn, timestamp, nonce, nonce_size))
  {
    memcpy (nn->nonce,
            nonce,
            nonce_size);
    nn->nonce[nonce_size] = 0;  /* With terminating zero */
    nn->nc = 0;
    nn->nmask = 0;
    ret = true;
  }
  else
    ret = false;
  MHD_mutex_unlock_chk_ (&daemon->nnc_lock);

  return ret;
}


/**
 * Calculate the server nonce so that it mitigates replay attacks and add
 * the new nonce to the nonce-nc map array.
 *
 * @param connection the MHD connection structure
 * @param realm A string of characters that describes the realm of auth.
 * @param da digest algorithm to use
 * @param[out] nonce A pointer to a character array for the nonce to put in,
 *        must provide NONCE_STD_LEN(da->digest_size)+1 bytes
 */
static bool
calculate_add_nonce_with_retry (struct MHD_Connection *const connection,
                                const char *realm,
                                struct DigestAlgorithm *da,
                                char *nonce)
{
  const uint64_t timestamp1 = MHD_monotonic_msec_counter ();
  const size_t realm_len = strlen (realm);

  if (! calculate_add_nonce (connection, timestamp1, realm, realm_len, da,
                             nonce))
  {
    /* Either:
     * 1. The same nonce was already generated. If it will be used then one
     * of the clients will fail (as no initial 'nc' value could be given to
     * the client, the second client which will use 'nc=00000001' will fail).
     * 2. Another nonce uses the same slot, and this nonce never has been
     * used by the client and this nonce is still fresh enough.
     */
    const size_t digest_size = digest_get_size (da);
    char nonce2[NONCE_STD_LEN (VLA_ARRAY_LEN_DIGEST (digest_size)) + 1];
    uint64_t timestamp2;
    if (0 == MHD_get_master (connection->daemon)->nonce_nc_size)
      return false; /* No need to re-try */

    timestamp2 = MHD_monotonic_msec_counter ();
    if (timestamp1 == timestamp2)
    {
      /* The timestamps are equal, need to generate some arbitrary
       * difference for nonce. */
      uint64_t base1;
      uint32_t base2;
      uint16_t base3;
      uint8_t base4;
      base1 = (uint64_t) (uintptr_t) nonce2;
      base2 = ((uint32_t) (base1 >> 32)) ^ ((uint32_t) base1);
      base2 = _MHD_ROTL32 (base2, 4);
      base3 = ((uint16_t) (base2 >> 16)) ^ ((uint16_t) base2);
      base4 = ((uint8_t) (base3 >> 8)) ^ ((uint8_t) base3);
      base1 = (uint64_t) (uintptr_t) connection;
      base2 = ((uint32_t) (base1 >> 32)) ^ ((uint32_t) base1);
      base2 = _MHD_ROTL32 (base2, (((base4 >> 4) ^ base4) % 32));
      base3 = ((uint16_t) (base2 >> 16)) ^ ((uint16_t) base2);
      base4 = ((uint8_t) (base3 >> 8)) ^ ((uint8_t) base3);
      /* Use up to 127 ms difference */
      timestamp2 -= (base4 & DAUTH_JUMPBACK_MAX);
      if (timestamp1 == timestamp2)
        timestamp2 -= 2; /* Fallback value */
    }
    if (! calculate_add_nonce (connection, timestamp2, realm, realm_len, da,
                               nonce2))
    {
      /* No free slot has been found. Re-tries are expensive, just use
       * the generated nonce. As it is not stored in nonce-nc map array,
       * the next request of the client will be recognized as valid, but 'stale'
       * so client should re-try automatically. */
      return false;
    }
    memcpy (nonce, nonce2, NONCE_STD_LEN (digest_size));
    mhd_assert (0 == nonce[NONCE_STD_LEN (digest_size)]);
  }
  return true;
}


/**
 * Test if the given key-value pair is in the headers for the
 * given connection.
 *
 * @param connection the connection
 * @param key the key
 * @param key_size number of bytes in @a key
 * @param value the value, can be NULL
 * @param value_size number of bytes in @a value
 * @param kind type of the header
 * @return #MHD_YES if the key-value pair is in the headers,
 *         #MHD_NO if not
 */
static enum MHD_Result
test_header (struct MHD_Connection *connection,
             const char *key,
             size_t key_size,
             const char *value,
             size_t value_size,
             enum MHD_ValueKind kind)
{
  struct MHD_HTTP_Req_Header *pos;

  for (pos = connection->headers_received; NULL != pos; pos = pos->next)
  {
    if (kind != pos->kind)
      continue;
    if (key_size != pos->header_size)
      continue;
    if (value_size != pos->value_size)
      continue;
    if (0 != memcmp (key,
                     pos->header,
                     key_size))
      continue;
    if ( (NULL == value) &&
         (NULL == pos->value) )
      return MHD_YES;
    if ( (NULL == value) ||
         (NULL == pos->value) ||
         (0 != memcmp (value,
                       pos->value,
                       value_size)) )
      continue;
    return MHD_YES;
  }
  return MHD_NO;
}


/**
 * Check that the arguments given by the client as part
 * of the authentication header match the arguments we
 * got as part of the HTTP request URI.
 *
 * @param connection connections with headers to compare against
 * @param args argument URI string (after "?" in URI)
 * @return #MHD_YES if the arguments match,
 *         #MHD_NO if not
 */
static enum MHD_Result
check_argument_match (struct MHD_Connection *connection,
                      const char *args)
{
  struct MHD_HTTP_Req_Header *pos;
  char *argb;
  unsigned int num_headers;
  enum MHD_Result ret;

  argb = strdup (args);
  if (NULL == argb)
  {
#ifdef HAVE_MESSAGES
    MHD_DLOG (connection->daemon,
              _ ("Failed to allocate memory for copy of URI arguments.\n"));
#endif /* HAVE_MESSAGES */
    return MHD_NO;
  }
  ret = MHD_parse_arguments_ (connection,
                              MHD_GET_ARGUMENT_KIND,
                              argb,
                              &test_header,
                              &num_headers);
  free (argb);
  if (MHD_NO == ret)
  {
    return MHD_NO;
  }
  /* also check that the number of headers matches */
  for (pos = connection->headers_received; NULL != pos; pos = pos->next)
  {
    if (MHD_GET_ARGUMENT_KIND != pos->kind)
      continue;
    num_headers--;
  }
  if (0 != num_headers)
  {
    /* argument count mismatch */
    return MHD_NO;
  }
  return MHD_YES;
}


/**
 * The size of the unquoting buffer in stack
 */
#define _MHD_STATIC_UNQ_BUFFER_SIZE 128

/**
 * The result of parameter unquoting
 */
enum _MHD_GetUnqResult
{
  _MHD_UNQ_NON_EMPTY = 0,                      /**< The sting is not empty */
  _MHD_UNQ_NO_STRING = MHD_DAUTH_WRONG_HEADER, /**< No string (no such parameter) */
  _MHD_UNQ_EMPTY = 1,                          /**< The string is empty */
  _MHD_UNQ_TOO_LARGE = 2,                      /**< The string is too large to unqoute */
  _MHD_UNQ_OUT_OF_MEM = 3                      /**< Out of memory error */
};


/**
 * Get Digest authorisation parameter as unquoted string.
 * @param param the parameter to process
 * @param tmp1 the small buffer in stack
 * @param ptmp2 the pointer to pointer to malloc'ed buffer
 * @param ptmp2_size the pointer to the size of the buffer pointed by @a ptmp2
 * @param[out] unquoted the pointer to store the result, NOT zero terminated
 * @return enum code indicating result of the process
 */
static enum _MHD_GetUnqResult
get_unqouted_param (const struct MHD_RqDAuthParam *param,
                    char tmp1[_MHD_STATIC_UNQ_BUFFER_SIZE],
                    char **ptmp2,
                    size_t *ptmp2_size,
                    struct _MHD_cstr_w_len *unquoted)
{
  char *str;
  size_t len;
  mhd_assert ((0 == *ptmp2_size) || (NULL != *ptmp2));
  mhd_assert ((NULL != *ptmp2) || (0 == *ptmp2_size));
  mhd_assert ((0 == *ptmp2_size) || \
              (_MHD_STATIC_UNQ_BUFFER_SIZE < *ptmp2_size));

  if (NULL == param->value.str)
  {
    const struct _MHD_cstr_w_len res = {NULL, 0};
    mhd_assert (! param->quoted);
    mhd_assert (0 == param->value.len);
    memcpy (unquoted, &res, sizeof(res));
    return _MHD_UNQ_NO_STRING;
  }
  if (! param->quoted)
  {
    memcpy (unquoted, &param->value, sizeof(param->value));
    return (0 == param->value.len) ? _MHD_UNQ_EMPTY : _MHD_UNQ_NON_EMPTY;
  }
  /* The value is present and is quoted, needs to be copied and unquoted */
  mhd_assert (0 != param->value.len);
  if (param->value.len <= _MHD_STATIC_UNQ_BUFFER_SIZE)
    str = tmp1;
  else if (param->value.len <= *ptmp2_size)
    str = *ptmp2;
  else
  {
    if (param->value.len > _MHD_AUTH_DIGEST_MAX_PARAM_SIZE)
      return _MHD_UNQ_TOO_LARGE;
    if (NULL != *ptmp2)
      free (*ptmp2);
    *ptmp2 = (char *) malloc (param->value.len);
    if (NULL == *ptmp2)
      return _MHD_UNQ_OUT_OF_MEM;
    *ptmp2_size = param->value.len;
    str = *ptmp2;
  }

  len = MHD_str_unquote (param->value.str, param->value.len, str);
  if (1)
  {
    const struct _MHD_cstr_w_len res = {str, len};
    memcpy (unquoted, &res, sizeof(res));
  }
  mhd_assert (0 != unquoted->len);
  mhd_assert (unquoted->len < param->value.len);
  return _MHD_UNQ_NON_EMPTY;
}


/**
 * Check whether Digest Auth request parameter is equal to given string
 * @param param the parameter to check
 * @param str the string to compare with, does not need to be zero-terminated
 * @param str_len the length of the @a str
 * @return true is parameter is equal to the given string,
 *         false otherwise
 */
_MHD_static_inline bool
is_param_equal (const struct MHD_RqDAuthParam *param,
                const char *const str,
                const size_t str_len)
{
  mhd_assert (NULL != param->value.str);
  mhd_assert (0 != param->value.len);
  if (param->quoted)
    return MHD_str_equal_quoted_bin_n (param->value.str, param->value.len,
                                       str, str_len);
  return (str_len == param->value.len) &&
         (0 == memcmp (str, param->value.str, str_len));

}


/**
 * Authenticates the authorization header sent by the client
 *
 * @param connection The MHD connection structure
 * @param[in,out] da digest algorithm to use for checking (written to as
 *         part of the calculations, but the values left in the struct
 *         are not actually expected to be useful for the caller)
 * @param realm The realm presented to the client
 * @param username The username needs to be authenticated
 * @param password The password used in the authentication
 * @param digest An optional binary hash
 *     of the precalculated hash value "username:realm:password"
 *     (must contain "da->digest_size" bytes or be NULL)
 * @param nonce_timeout The amount of time for a nonce to be
 *      invalid in seconds
 * @return #MHD_DAUTH_OK if authenticated,
 *         error code otherwise.
 * @ingroup authentication
 */
static enum MHD_DigestAuthResult
digest_auth_check_all (struct MHD_Connection *connection,
                       struct DigestAlgorithm *da,
                       const char *realm,
                       const char *username,
                       const char *password,
                       const uint8_t *digest,
                       unsigned int nonce_timeout)
{
  struct MHD_Daemon *daemon = MHD_get_master (connection->daemon);
  char cnonce[MAX_CLIENT_NONCE_LENGTH];
  const unsigned int digest_size = digest_get_size (da);
  char ha1[VLA_ARRAY_LEN_DIGEST (digest_size) * 2 + 1];
  char qop[15]; /* auth,auth-int */
  char nc[20];
  char response[MAX_AUTH_RESPONSE_LENGTH];
  const char *hentity = NULL; /* "auth-int" is not supported */
  char noncehashexp[NONCE_STD_LEN (VLA_ARRAY_LEN_DIGEST (digest_size)) + 1];
  uint64_t nonce_time;
  uint64_t t;
  uint64_t nci;
  char *qmark;
  const struct MHD_RqDAuth *params;
  char tmp1[_MHD_STATIC_UNQ_BUFFER_SIZE]; /**< Temporal buffer in stack for unqouting */
  char *tmp2;     /**< Temporal malloc'ed buffer for unqouting */
  size_t tmp2_size; /**< The size of @a tmp2 buffer */
  struct _MHD_cstr_w_len unquoted;
  enum _MHD_GetUnqResult unq_res;
  enum MHD_DigestAuthResult ret;
#ifdef HAVE_MESSAGES
  bool err_logged;
#endif /* HAVE_MESSAGES */
  size_t username_len;
  size_t realm_len;

  tmp2 = NULL;
  tmp2_size = 0;
#ifdef HAVE_MESSAGES
  err_logged = false;
#endif /* HAVE_MESSAGES */

  do /* Only to avoid "goto" */
  {

    params = get_rq_dauth_params (connection);
    if (NULL == params)
    {
      ret = MHD_DAUTH_WRONG_HEADER;
      break;
    }

    /* Check 'username' */
    if (NULL == params->username.value.str)
    {
      ret = MHD_DAUTH_WRONG_HEADER;
      break;
    }
    username_len = strlen (username);
    if (! is_param_equal (&params->username, username, username_len))
    {
      ret = MHD_DAUTH_WRONG_USERNAME;
      break;
    }
    /* 'username' valid */

    /* Check 'realm' */
    if (NULL == params->realm.value.str)
    {
      ret = MHD_DAUTH_WRONG_HEADER;
      break;
    }
    realm_len = strlen (realm);
    if (! is_param_equal (&params->realm, realm, realm_len))
    {
      ret = MHD_DAUTH_WRONG_REALM;
      break;
    }
    /* 'realm' valid */

    /* Check 'nonce' */
    unq_res = get_unqouted_param (&params->nonce, tmp1, &tmp2, &tmp2_size,
                                  &unquoted);
    if (_MHD_UNQ_NON_EMPTY != unq_res)
    {
      if (_MHD_UNQ_NO_STRING == unq_res)
        ret = MHD_DAUTH_WRONG_HEADER;
      else if (_MHD_UNQ_EMPTY == unq_res)
        ret = MHD_DAUTH_NONCE_WRONG;
      else if (_MHD_UNQ_TOO_LARGE == unq_res)
        ret = MHD_DAUTH_WRONG_HEADER;
      else if (_MHD_UNQ_OUT_OF_MEM == unq_res)
        ret = MHD_DAUTH_ERROR;
      else
      {
        mhd_assert (0); /* Must not happen */
        ret = MHD_DAUTH_ERROR;
      }
      break;
    }
    /* TODO: check correct 'nonce' length */
    if (! get_nonce_timestamp (unquoted.str, unquoted.len, &nonce_time))
    {
#ifdef HAVE_MESSAGES
      MHD_DLOG (daemon,
                _ ("Authentication failed, invalid timestamp format.\n"));
      err_logged = true;
#endif
      ret = MHD_DAUTH_NONCE_WRONG;
      break;
    }
    t = MHD_monotonic_msec_counter ();
    /*
     * First level vetting for the nonce validity: if the timestamp
     * attached to the nonce exceeds `nonce_timeout', then the nonce is
     * invalid.
     */
    if (TRIM_TO_TIMESTAMP (t - nonce_time) > (nonce_timeout * 1000))
    {
      /* too old */
      ret = MHD_DAUTH_NONCE_STALE;
      break;
    }

    calculate_nonce (nonce_time,
                     connection->method,
                     daemon->digest_auth_random,
                     daemon->digest_auth_rand_size,
                     connection->url,
                     realm,
                     realm_len,
                     da,
                     noncehashexp);
    /*
     * Second level vetting for the nonce validity
     * if the timestamp attached to the nonce is valid
     * and possibly fabricated (in case of an attack)
     * the attacker must also know the random seed to be
     * able to generate a "sane" nonce, which if he does
     * not, the nonce fabrication process going to be
     * very hard to achieve.
     */
    if ((0 != strncmp (noncehashexp, unquoted.str, unquoted.len)) ||
        (0 != noncehashexp[unquoted.len]))
    {
      ret = MHD_DAUTH_NONCE_WRONG;
      break;
    }
    /* 'nonce' valid */

    /* Get 'cnonce' */
    unq_res = get_unqouted_param (&params->cnonce, tmp1, &tmp2, &tmp2_size,
                                  &unquoted);
    if (_MHD_UNQ_NON_EMPTY != unq_res)
    {
      if (_MHD_UNQ_NO_STRING == unq_res)
        ret = MHD_DAUTH_WRONG_HEADER;
      else if (_MHD_UNQ_EMPTY == unq_res)
        ret = MHD_DAUTH_WRONG_HEADER;
      else if (_MHD_UNQ_TOO_LARGE == unq_res)
        ret = MHD_DAUTH_WRONG_HEADER;
      else if (_MHD_UNQ_OUT_OF_MEM == unq_res)
        ret = MHD_DAUTH_ERROR;
      else
      {
        mhd_assert (0); /* Must not happen */
        ret = MHD_DAUTH_ERROR;
      }
      break;
    }
    if (sizeof(cnonce) <= unquoted.len)
    {
      /* TODO: handle large client nonces */
      ret = MHD_DAUTH_ERROR;
      break;
    }
    /* TODO: avoid memcpy() */
    memcpy (cnonce, unquoted.str, unquoted.len);
    cnonce[unquoted.len] = 0;
    /* Got 'cnonce' */

    /* Get 'qop' */
    unq_res = get_unqouted_param (&params->qop, tmp1, &tmp2, &tmp2_size,
                                  &unquoted);
    if (_MHD_UNQ_NON_EMPTY != unq_res)
    {
      if (_MHD_UNQ_NO_STRING == unq_res)
        ret = MHD_DAUTH_WRONG_HEADER;
      else if (_MHD_UNQ_EMPTY == unq_res)
        ret = MHD_DAUTH_WRONG_HEADER;
      else if (_MHD_UNQ_TOO_LARGE == unq_res)
        ret = MHD_DAUTH_WRONG_HEADER;
      else if (_MHD_UNQ_OUT_OF_MEM == unq_res)
        ret = MHD_DAUTH_ERROR;
      else
      {
        mhd_assert (0); /* Must not happen */
        ret = MHD_DAUTH_ERROR;
      }
      break;
    }
    if (sizeof(qop) <= unquoted.len)
    {
      /* TODO: handle large client qop */
      ret = MHD_DAUTH_ERROR;
      break;
    }
    /* TODO: avoid memcpy() */
    memcpy (qop, unquoted.str, unquoted.len);
    qop[unquoted.len] = 0;
    /* TODO: use caseless match, use dedicated return code */
    if ( (0 != strcmp (qop, "auth")) &&
         (0 != strcmp (qop,"")) )
    {
      ret = MHD_DAUTH_WRONG_HEADER;
      break;
    }
    /* Got 'qop' */

    /* Get 'nc' */
    unq_res = get_unqouted_param (&params->nc, tmp1, &tmp2, &tmp2_size,
                                  &unquoted);
    if (_MHD_UNQ_NON_EMPTY != unq_res)
    {
      if (_MHD_UNQ_NO_STRING == unq_res)
        ret = MHD_DAUTH_WRONG_HEADER;
      else if (_MHD_UNQ_EMPTY == unq_res)
        ret = MHD_DAUTH_WRONG_HEADER;
      else if (_MHD_UNQ_TOO_LARGE == unq_res)
        ret = MHD_DAUTH_WRONG_HEADER;
      else if (_MHD_UNQ_OUT_OF_MEM == unq_res)
        ret = MHD_DAUTH_ERROR;
      else
      {
        mhd_assert (0); /* Must not happen */
        ret = MHD_DAUTH_ERROR;
      }
      break;
    }
    if (sizeof(nc) <= unquoted.len)
    {
      /* TODO: handle large client nc */
      ret = MHD_DAUTH_ERROR;
      break;
    }
    /* TODO: avoid memcpy() */
    memcpy (nc, unquoted.str, unquoted.len);
    nc[unquoted.len] = 0;
    if (unquoted.len != MHD_strx_to_uint64_n_ (nc,
                                               unquoted.len,
                                               &nci))
    {
#ifdef HAVE_MESSAGES
      MHD_DLOG (daemon,
                _ ("Authentication failed, invalid nc format.\n"));
      err_logged = true;
#endif
      ret = MHD_DAUTH_WRONG_HEADER;   /* invalid nonce format */
      break;
    }
    if (0 == nci)
    {
#ifdef HAVE_MESSAGES
      MHD_DLOG (daemon,
                _ ("Authentication failed, invalid 'nc' value.\n"));
      err_logged = true;
#endif
      ret = MHD_DAUTH_WRONG_HEADER;   /* invalid nc value */
      break;
    }
    /* Got 'nc' */

    /* Get 'response' */
    unq_res = get_unqouted_param (&params->response, tmp1, &tmp2, &tmp2_size,
                                  &unquoted);
    if (_MHD_UNQ_NON_EMPTY != unq_res)
    {
      if (_MHD_UNQ_NO_STRING == unq_res)
        ret = MHD_DAUTH_WRONG_HEADER;
      else if (_MHD_UNQ_EMPTY == unq_res)
        ret = MHD_DAUTH_WRONG_HEADER;
      else if (_MHD_UNQ_TOO_LARGE == unq_res)
        ret = MHD_DAUTH_WRONG_HEADER;
      else if (_MHD_UNQ_OUT_OF_MEM == unq_res)
        ret = MHD_DAUTH_ERROR;
      else
      {
        mhd_assert (0); /* Must not happen */
        ret = MHD_DAUTH_ERROR;
      }
      break;
    }
    if (sizeof(response) <= unquoted.len)
    {
      /* TODO: handle large client response */
      ret = MHD_DAUTH_ERROR;
      break;
    }
    /* TODO: avoid memcpy() */
    memcpy (response, unquoted.str, unquoted.len);
    response[unquoted.len] = 0;
    /* Got 'response' */

    if (1)
    {
      enum MHD_CheckNonceNC_ nonce_nc_check;
      /*
       * Checking if that combination of nonce and nc is sound
       * and not a replay attack attempt. Refuse if nonce was not
       * generated previously.
       */
      nonce_nc_check = check_nonce_nc (connection,
                                       noncehashexp,
                                       NONCE_STD_LEN (digest_size),
                                       nonce_time,
                                       nci);
      if (MHD_CHECK_NONCENC_STALE == nonce_nc_check)
      {
#ifdef HAVE_MESSAGES
        MHD_DLOG (daemon,
                  _ ("Stale nonce received. If this happens a lot, you should "
                     "probably increase the size of the nonce array.\n"));
        err_logged = true;
#endif
        ret = MHD_DAUTH_NONCE_STALE;
        break;
      }
      else if (MHD_CHECK_NONCENC_WRONG == nonce_nc_check)
      {
#ifdef HAVE_MESSAGES
        MHD_DLOG (daemon,
                  _ ("Received nonce that technically valid, but was not "
                     "generated by MHD. This may indicate an attack attempt.\n"));
        err_logged = true;
#endif
        ret = MHD_DAUTH_NONCE_WRONG;
        break;
      }
      mhd_assert (MHD_CHECK_NONCENC_OK == nonce_nc_check);
    }

    /* Get 'uri' */
    unq_res = get_unqouted_param (&params->uri, tmp1, &tmp2, &tmp2_size,
                                  &unquoted);
    if (_MHD_UNQ_NON_EMPTY != unq_res)
    {
      if (_MHD_UNQ_NO_STRING == unq_res)
        ret = MHD_DAUTH_WRONG_HEADER;
      else if (_MHD_UNQ_EMPTY == unq_res)
        ret = MHD_DAUTH_WRONG_HEADER;
      else if (_MHD_UNQ_TOO_LARGE == unq_res)
        ret = MHD_DAUTH_WRONG_HEADER;
      else if (_MHD_UNQ_OUT_OF_MEM == unq_res)
        ret = MHD_DAUTH_ERROR;
      else
      {
        mhd_assert (0); /* Must not happen */
        ret = MHD_DAUTH_ERROR;
      }
      break;
    }
    if (NULL != digest)
    {
      /* This will initialize da->digest_hex (ha1) */
      digest_calc_ha1_from_digest (digest_get_algo_name (da),
                                   da,
                                   digest,
                                   noncehashexp,
                                   cnonce);
    }
    else
    {
      /* This will initialize da->digest_hex (ha1) */
      mhd_assert (NULL != password);   /* NULL == digest => password != NULL */
      digest_calc_ha1_from_user (digest_get_algo_name (da),
                                 username,
                                 username_len,
                                 realm,
                                 realm_len,
                                 password,
                                 noncehashexp,
                                 cnonce,
                                 da);
    }
    memcpy (ha1,
            digest_get_hex_buffer (da),
            digest_size * 2 + 1);
    /* This will initialize da->sessionkey (respexp) */
    digest_calc_response (ha1,
                          noncehashexp,
                          nc,
                          cnonce,
                          qop,
                          connection->method,
                          unquoted.str,
                          unquoted.len,
                          hentity,
                          da);
    if (1)
    {
      char *uri;
      size_t uri_len;
      uri_len = unquoted.len;
      /* TODO: simplify string copy, avoid potential double copy */
      if ( ((tmp1 != unquoted.str) && (tmp2 != unquoted.str)) ||
           ((tmp1 == unquoted.str) && (sizeof(tmp1) >= unquoted.len)) ||
           ((tmp2 == unquoted.str) && (tmp2_size >= unquoted.len)))
      {
        char *buf;
        mhd_assert ((tmp1 != unquoted.str) || \
                    (sizeof(tmp1) == unquoted.len));
        mhd_assert ((tmp2 != unquoted.str) || \
                    (tmp2_size == unquoted.len));
        buf = malloc (unquoted.len + 1);
        if (NULL == buf)
        {
          ret = MHD_DAUTH_ERROR;
          break;
        }
        memcpy (buf, unquoted.str, unquoted.len);
        if (NULL != tmp2)
          free (tmp2);
        tmp2 = buf;
        tmp2_size = unquoted.len + 1;
        uri = tmp2;
      }
      else if (tmp1 == unquoted.str)
        uri = tmp1;
      else
      {
        mhd_assert (tmp2 == unquoted.str);
        uri = tmp2;
      }

      uri[uri_len] = 0;
      qmark = memchr (uri,
                      '?',
                      uri_len);
      if (NULL != qmark)
        *qmark = '\0';

      /* Need to unescape URI before comparing with connection->url */
      daemon->unescape_callback (daemon->unescape_callback_cls,
                                 connection,
                                 uri);
      if (0 != strcmp (uri,
                       connection->url))
      {
#ifdef HAVE_MESSAGES
        MHD_DLOG (daemon,
                  _ ("Authentication failed, URI does not match.\n"));
        err_logged = true;
#endif
        ret = MHD_DAUTH_WRONG_URI;
        break;
      }

      if (1)
      {
        const char *args = qmark;

        if (NULL == args)
          args = "";
        else
          args++;
        if (MHD_NO ==
            check_argument_match (connection,
                                  args) )
        {
#ifdef HAVE_MESSAGES
          MHD_DLOG (daemon,
                    _ ("Authentication failed, arguments do not match.\n"));
          err_logged = true;
#endif
          ret = MHD_DAUTH_WRONG_URI;
          break;
        }
      }

      ret = (0 == strcmp (response,
                          digest_get_hex_buffer (da)))
      ? MHD_DAUTH_OK
      : MHD_DAUTH_RESPONSE_WRONG;
    }
  } while (0);
  if (NULL != tmp2)
    free (tmp2);

  if ((MHD_DAUTH_OK != ret) && ! err_logged)
  {
    (void) 0; /* TODO: add logging */
  }

  return ret;
}


/**
 * Authenticates the authorization header sent by the client.
 * Uses #MHD_DIGEST_ALG_MD5 (for now, for backwards-compatibility).
 * Note that this MAY change to #MHD_DIGEST_ALG_AUTO in the future.
 * If you want to be sure you get MD5, use #MHD_digest_auth_check2()
 * and specify MD5 explicitly.
 *
 * @param connection The MHD connection structure
 * @param realm The realm presented to the client
 * @param username The username needs to be authenticated
 * @param password The password used in the authentication
 * @param nonce_timeout The amount of time for a nonce to be
 *      invalid in seconds
 * @return #MHD_YES if authenticated, #MHD_NO if not,
 *         #MHD_INVALID_NONCE if nonce is invalid or stale
 * @deprecated use MHD_digest_auth_check3()
 * @ingroup authentication
 */
_MHD_EXTERN int
MHD_digest_auth_check (struct MHD_Connection *connection,
                       const char *realm,
                       const char *username,
                       const char *password,
                       unsigned int nonce_timeout)
{
  return MHD_digest_auth_check2 (connection,
                                 realm,
                                 username,
                                 password,
                                 nonce_timeout,
                                 MHD_DIGEST_ALG_MD5);
}


/**
 * Authenticates the authorization header sent by the client.
 *
 * @param connection the MHD connection structure
 * @param realm the realm to be used for authorization of the client
 * @param username the username needs to be authenticated
 * @param password the password used in the authentication
 * @param nonce_timeout the nonce validity duration in seconds
 * @param algo the digest algorithms allowed for verification
 * @return #MHD_DAUTH_OK if authenticated,
 *         the error code otherwise
 * @note Available since #MHD_VERSION 0x00097513
 * @ingroup authentication
 */
_MHD_EXTERN enum MHD_DigestAuthResult
MHD_digest_auth_check3 (struct MHD_Connection *connection,
                        const char *realm,
                        const char *username,
                        const char *password,
                        unsigned int nonce_timeout,
                        enum MHD_DigestAuthAlgorithm algo)
{
  struct DigestAlgorithm da;

  mhd_assert (NULL != password);

  if (! digest_setup (&da, algo))
    MHD_PANIC (_ ("Wrong algo value.\n")); /* API violation! */

  return digest_auth_check_all (connection,
                                &da,
                                realm,
                                username,
                                password,
                                NULL,
                                nonce_timeout);
}


/**
 * Authenticates the authorization header sent by the client.
 *
 * @param connection the MHD connection structure
 * @param realm the realm to be used for authorization of the client
 * @param username the username needs to be authenticated
 * @param digest the pointer to the binary digest for the precalculated hash
 *        value "username:realm:password" with specified @a algo
 * @param digest_size the number of bytes in @a digest (the size must match
 *        @a algo!)
 * @param nonce_timeout the nonce validity duration in seconds
 * @param algo digest algorithms allowed for verification
 * @return #MHD_DAUTH_OK if authenticated,
 *         the error code otherwise
 * @note Available since #MHD_VERSION 0x00097513
 * @ingroup authentication
 */
_MHD_EXTERN enum MHD_DigestAuthResult
MHD_digest_auth_check_digest3 (struct MHD_Connection *connection,
                               const char *realm,
                               const char *username,
                               const uint8_t *digest,
                               size_t digest_size,
                               unsigned int nonce_timeout,
                               enum MHD_DigestAuthAlgorithm algo)
{
  struct DigestAlgorithm da;

  mhd_assert (NULL != digest);
  if (! digest_setup (&da, algo))
    MHD_PANIC (_ ("Wrong algo value.\n")); /* API violation! */

  if (digest_get_size (&da) != digest_size)
    MHD_PANIC (_ ("Digest size mismatch.\n")); /* API violation! */

  return digest_auth_check_all (connection,
                                &da,
                                realm,
                                username,
                                NULL,
                                digest,
                                nonce_timeout);
}


/**
 * Authenticates the authorization header sent by the client.
 *
 * @param connection The MHD connection structure
 * @param realm The realm presented to the client
 * @param username The username needs to be authenticated
 * @param password The password used in the authentication
 * @param nonce_timeout The amount of time for a nonce to be
 *      invalid in seconds
 * @param algo digest algorithms allowed for verification
 * @return #MHD_YES if authenticated, #MHD_NO if not,
 *         #MHD_INVALID_NONCE if nonce is invalid or stale
 * @note Available since #MHD_VERSION 0x00096200
 * @deprecated use MHD_digest_auth_check3()
 * @ingroup authentication
 */
_MHD_EXTERN int
MHD_digest_auth_check2 (struct MHD_Connection *connection,
                        const char *realm,
                        const char *username,
                        const char *password,
                        unsigned int nonce_timeout,
                        enum MHD_DigestAuthAlgorithm algo)
{
  enum MHD_DigestAuthResult res;
  res = MHD_digest_auth_check3 (connection,
                                realm,
                                username,
                                password,
                                nonce_timeout,
                                algo);
  if (MHD_DAUTH_OK == res)
    return MHD_YES;
  else if ((MHD_DAUTH_NONCE_STALE == res) || (MHD_DAUTH_NONCE_WRONG == res))
    return MHD_INVALID_NONCE;
  return MHD_NO;

}


/**
 * Authenticates the authorization header sent by the client.
 *
 * @param connection The MHD connection structure
 * @param realm The realm presented to the client
 * @param username The username needs to be authenticated
 * @param digest An `unsigned char *' pointer to the binary MD5 sum
 *      for the precalculated hash value "username:realm:password"
 *      of @a digest_size bytes
 * @param digest_size number of bytes in @a digest (size must match @a algo!)
 * @param nonce_timeout The amount of time for a nonce to be
 *      invalid in seconds
 * @param algo digest algorithms allowed for verification
 * @return #MHD_YES if authenticated, #MHD_NO if not,
 *         #MHD_INVALID_NONCE if nonce is invalid or stale
 * @note Available since #MHD_VERSION 0x00096200
 * @deprecated use MHD_digest_auth_check_digest3()
 * @ingroup authentication
 */
_MHD_EXTERN int
MHD_digest_auth_check_digest2 (struct MHD_Connection *connection,
                               const char *realm,
                               const char *username,
                               const uint8_t *digest,
                               size_t digest_size,
                               unsigned int nonce_timeout,
                               enum MHD_DigestAuthAlgorithm algo)
{
  enum MHD_DigestAuthResult res;

  res = MHD_digest_auth_check_digest3 (connection,
                                       realm,
                                       username,
                                       digest,
                                       digest_size,
                                       nonce_timeout,
                                       algo);
  if (MHD_DAUTH_OK == res)
    return MHD_YES;
  else if ((MHD_DAUTH_NONCE_STALE == res) || (MHD_DAUTH_NONCE_WRONG == res))
    return MHD_INVALID_NONCE;
  return MHD_NO;
}


/**
 * Authenticates the authorization header sent by the client
 * Uses #MHD_DIGEST_ALG_MD5 (required, as @a digest is of fixed
 * size).
 *
 * @param connection The MHD connection structure
 * @param realm The realm presented to the client
 * @param username The username needs to be authenticated
 * @param digest An `unsigned char *' pointer to the binary hash
 *    for the precalculated hash value "username:realm:password";
 *    length must be #MHD_MD5_DIGEST_SIZE bytes
 * @param nonce_timeout The amount of time for a nonce to be
 *      invalid in seconds
 * @return #MHD_YES if authenticated, #MHD_NO if not,
 *         #MHD_INVALID_NONCE if nonce is invalid or stale
 * @note Available since #MHD_VERSION 0x00096000
 * @deprecated use #MHD_digest_auth_check_digest3()
 * @ingroup authentication
 */
_MHD_EXTERN int
MHD_digest_auth_check_digest (struct MHD_Connection *connection,
                              const char *realm,
                              const char *username,
                              const uint8_t digest[MHD_MD5_DIGEST_SIZE],
                              unsigned int nonce_timeout)
{
  return MHD_digest_auth_check_digest2 (connection,
                                        realm,
                                        username,
                                        digest,
                                        MHD_MD5_DIGEST_SIZE,
                                        nonce_timeout,
                                        MHD_DIGEST_ALG_MD5);
}


/**
 * Queues a response to request authentication from the client
 *
 * @param connection The MHD connection structure
 * @param realm the realm presented to the client
 * @param opaque string to user for opaque value
 * @param response reply to send; should contain the "access denied"
 *        body; note that this function will set the "WWW Authenticate"
 *        header and that the caller should not do this; the NULL is tolerated
 * @param signal_stale #MHD_YES if the nonce is stale to add
 *        'stale=true' to the authentication header
 * @param algo digest algorithm to use
 * @return #MHD_YES on success, #MHD_NO otherwise
 * @note Available since #MHD_VERSION 0x00096200
 * @ingroup authentication
 */
_MHD_EXTERN enum MHD_Result
MHD_queue_auth_fail_response2 (struct MHD_Connection *connection,
                               const char *realm,
                               const char *opaque,
                               struct MHD_Response *response,
                               int signal_stale,
                               enum MHD_DigestAuthAlgorithm algo)
{
  enum MHD_Result ret;
  int hlen;

  struct DigestAlgorithm da;

  if (! digest_setup (&da, algo))
    MHD_PANIC (_ ("Wrong algo value.\n")); /* API violation! */

  if (NULL == response)
    return MHD_NO;

  if (0 == MHD_get_master (connection->daemon)->nonce_nc_size)
  {
#ifdef HAVE_MESSAGES
    MHD_DLOG (connection->daemon,
              _ ("The nonce array size is zero.\n"));
#endif /* HAVE_MESSAGES */
    return MHD_NO;
  }

  if (1)
  {
    char nonce[NONCE_STD_LEN (VLA_ARRAY_LEN_DIGEST (digest_get_size (&da)))
               + 1];

    VLA_CHECK_LEN_DIGEST (digest_get_size (&da));
    if (! calculate_add_nonce_with_retry (connection, realm, &da, nonce))
    {
#ifdef HAVE_MESSAGES
      MHD_DLOG (connection->daemon,
                _ ("Could not register nonce. Client's requests with this "
                   "nonce will be always 'stale'. Probably clients' requests "
                   "are too intensive.\n"));
#else  /* ! HAVE_MESSAGES */
      (void) 0;
#endif /* ! HAVE_MESSAGES */
    }
    /* Building the authentication header */
    hlen = MHD_snprintf_ (NULL,
                          0,
                          "Digest realm=\"%s\",qop=\"auth\",nonce=\"%s\",opaque=\"%s\",algorithm=%s%s",
                          realm,
                          nonce,
                          opaque,
                          digest_get_algo_name (&da),
                          signal_stale
                          ? ",stale=\"true\""
                          : "");
    if (hlen > 0)
    {
      char *header;

      header = MHD_calloc_ (1,
                            (size_t) hlen + 1);
      if (NULL == header)
      {
#ifdef HAVE_MESSAGES
        MHD_DLOG (connection->daemon,
                  _ ("Failed to allocate memory for auth response header.\n"));
#endif /* HAVE_MESSAGES */
        return MHD_NO;
      }

      if (MHD_snprintf_ (header,
                         (size_t) hlen + 1,
                         "Digest realm=\"%s\",qop=\"auth\",nonce=\"%s\",opaque=\"%s\",algorithm=%s%s",
                         realm,
                         nonce,
                         opaque,
                         digest_get_algo_name (&da),
                         signal_stale
                         ? ",stale=\"true\""
                         : "") == hlen)
        ret = MHD_add_response_header (response,
                                       MHD_HTTP_HEADER_WWW_AUTHENTICATE,
                                       header);
      else
        ret = MHD_NO;
#if 0
      if ( (MHD_NO != ret) && (AND in state : 100 continue aborting ...))
        ret = MHD_add_response_header (response,
                                       MHD_HTTP_HEADER_CONNECTION,
                                       "close");
#endif
      free (header);
    }
    else
      ret = MHD_NO;
  }

  if (MHD_NO != ret)
  {
    ret = MHD_queue_response (connection,
                              MHD_HTTP_UNAUTHORIZED,
                              response);
  }
  else
  {
#ifdef HAVE_MESSAGES
    MHD_DLOG (connection->daemon,
              _ ("Failed to add Digest auth header.\n"));
#endif /* HAVE_MESSAGES */
  }
  return ret;
}


/**
 * Queues a response to request authentication from the client.
 * For now uses MD5 (for backwards-compatibility). Still, if you
 * need to be sure, use #MHD_queue_auth_fail_response2().
 *
 * @param connection The MHD connection structure
 * @param realm the realm presented to the client
 * @param opaque string to user for opaque value
 * @param response reply to send; should contain the "access denied"
 *        body; note that this function will set the "WWW Authenticate"
 *        header and that the caller should not do this; the NULL is tolerated
 * @param signal_stale #MHD_YES if the nonce is stale to add
 *        'stale=true' to the authentication header
 * @return #MHD_YES on success, #MHD_NO otherwise
 * @ingroup authentication
 * @deprecated use MHD_queue_auth_fail_response2()
 */
_MHD_EXTERN enum MHD_Result
MHD_queue_auth_fail_response (struct MHD_Connection *connection,
                              const char *realm,
                              const char *opaque,
                              struct MHD_Response *response,
                              int signal_stale)
{
  return MHD_queue_auth_fail_response2 (connection,
                                        realm,
                                        opaque,
                                        response,
                                        signal_stale,
                                        MHD_DIGEST_ALG_MD5);
}


/* end of digestauth.c */
