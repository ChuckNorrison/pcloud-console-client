/*
 * This file is part of the pCloud Console Client.
 *
 * (c) 2021 Serghei Iakovlev <egrep@protonmail.ch>
 * (c) 2015 Anton Titov <anton@pcloud.com>
 * (c) 2015 pCloud Ltd
 *
 * For the full copyright and license information, please view
 * the LICENSE file that was distributed with this source code.
 */

#include <pthread.h>
#include <ctype.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ssl.h>
#include <mbedtls/pkcs5.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/sha256.h>
#include <mbedtls/error.h>

#include "plibs.h"
#include "pssl.h"
#include "psynclib.h"
#include "psslcerts.h"
#include "psettings.h"
#include "pcache.h"
#include "ptimer.h"
#include "pmemlock.h"
#include "pssl-mbedtls.h"

#ifdef PSYNC_AES_HW_MSC
#include <intrin.h>
#include <wmmintrin.h>
#endif  /* PSYNC_AES_HW_MSC */

static const int psync_mbed_ciphersuite[] = {
  MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
  MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
  MBEDTLS_TLS_DHE_RSA_WITH_AES_256_GCM_SHA384,
  MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_CBC_SHA384,
  MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA384,
  MBEDTLS_TLS_DHE_RSA_WITH_AES_256_CBC_SHA256,
  MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
  MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
  MBEDTLS_TLS_DHE_RSA_WITH_AES_128_GCM_SHA256,
  MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256,
  MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256,
  MBEDTLS_TLS_DHE_RSA_WITH_AES_128_CBC_SHA256,
  0
};

typedef struct {
  mbedtls_ctr_drbg_context rnd;
  pthread_mutex_t mutex;
} ctr_drbg_context_locked;


typedef struct {
  mbedtls_ssl_context ssl;
  mbedtls_ssl_config config;
  psync_socket_t sock;
  int isbroken;
  char cachekey[];
} ssl_connection_t;

static ctr_drbg_context_locked psync_mbed_rng;
static mbedtls_entropy_context psync_mbed_entropy;
static mbedtls_x509_crt psync_mbed_trusted_certs_x509;

PSYNC_THREAD int psync_ssl_errno;

#if defined(PSYNC_AES_HW)
uint32_t psync_ssl_hw_aes;
#endif

int ctr_drbg_random_locked(void *p_rng, unsigned char *output, size_t output_len) {
  ctr_drbg_context_locked *rng;
  int ret;

  rng=(ctr_drbg_context_locked *)p_rng;
  pthread_mutex_lock(&rng->mutex);
  ret=mbedtls_ctr_drbg_random(&rng->rnd, output, output_len);
  pthread_mutex_unlock(&rng->mutex);
  return ret;
}

#ifdef PSYNC_AES_HW_GCC /* TODO: This should work on Clang too */
static uint32_t psync_ssl_detect_aes_hw() {
  uint32_t eax, ecx;
  eax = 1;
  __asm__ ("cpuid"
         : "=c" (ecx)
         : "a" (eax)
         : "%ebx", "%edx");
  ecx = (ecx >> 25) & 1;
  if (ecx)
    log_info("hardware AES support detected");
  else
    log_info("hardware AES support not detected");
  return ecx;
}
#elif defined(PSYNC_AES_HW_MSC)
static uint32_t psync_ssl_detect_aes_hw() {
  int info[4];
  uint32_t ret;
  __cpuid(info, 1);
  ret = (info[2] >> 25) & 1;
  if (ret)
    log_info("hardware AES support detected");
  else
    log_info("hardware AES support not detected");
  return ret;
}
#endif

int psync_ssl_init() {
  unsigned char seed[PSYNC_LHASH_DIGEST_LEN];
  psync_uint_t i;
#if defined(PSYNC_AES_HW)
  psync_ssl_hw_aes=psync_ssl_detect_aes_hw();
#else
  log_info("hardware AES is not supported for this compiler");
#endif
  if (pthread_mutex_init(&psync_mbed_rng.mutex, NULL))
    return -1;

  mbedtls_entropy_init(&psync_mbed_entropy);
  psync_get_random_seed(seed, seed, sizeof(seed), 0);
  mbedtls_entropy_update_manual(&psync_mbed_entropy, seed, sizeof(seed));

  /* Initialize the CTR_DRBG context */
  mbedtls_ctr_drbg_init(&psync_mbed_rng.rnd);

  /* Seed and set up the CTR_DRBG entropy source for future reseeds */
  int seed_status = mbedtls_ctr_drbg_seed(
      &psync_mbed_rng.rnd,
      mbedtls_entropy_func,
      &psync_mbed_entropy,
      NULL,
      0
  );
  if (seed_status != 0) {
    return -1;
  }

  mbedtls_x509_crt_init(&psync_mbed_trusted_certs_x509);

  /* Loading the certificates */
  int crt_parse_status;
  for (i = 0; i < ARRAY_SIZE(psync_ssl_trusted_certs); ++i) {
    crt_parse_status = mbedtls_x509_crt_parse(
        &psync_mbed_trusted_certs_x509,
        (const unsigned char *)psync_ssl_trusted_certs[i],
        strlen(psync_ssl_trusted_certs[i]) + 1
    );

    if (crt_parse_status != 0) {
      mbedtls_log_error(crt_parse_status);
    }
  }

  return 0;
}

void psync_ssl_memclean(void *ptr, size_t len) {
  volatile unsigned char *p=ptr;
  while (len--)
    *p++=0;
}

static ssl_connection_t *psync_ssl_alloc_conn(const char *hostname) {
  ssl_connection_t *conn;
  size_t len;
  len=strlen(hostname)+1;
  conn=(ssl_connection_t *)psync_malloc(offsetof(ssl_connection_t, cachekey)+len+4);
  conn->isbroken=0;
  memcpy(conn->cachekey, "SSLS", 4);
  memcpy(conn->cachekey+4, hostname, len);
  return conn;
}

static void psync_set_ssl_error(ssl_connection_t *conn, int err) {
  if (err == MBEDTLS_ERR_SSL_WANT_READ)
    psync_ssl_errno = PSYNC_SSL_ERR_WANT_READ;
  else if (err == MBEDTLS_ERR_SSL_WANT_WRITE)
    psync_ssl_errno = PSYNC_SSL_ERR_WANT_WRITE;
  else {
    psync_ssl_errno = PSYNC_SSL_ERR_UNKNOWN;
    conn->isbroken = 1;

    if (err == MBEDTLS_ERR_NET_RECV_FAILED)
      log_debug("got MBEDTLS_ERR_NET_RECV_FAILED");
    else if (err == MBEDTLS_ERR_NET_SEND_FAILED)
      log_debug("got MBEDTLS_ERR_NET_SEND_FAILED");
    else
      log_debug("got error %d", err);
  }
}

static int psync_mbed_read(void *ptr, unsigned char *buf, size_t len) {
  ssl_connection_t *conn;
  ssize_t ret;
  int err;
  conn=(ssl_connection_t *)ptr;
  ret=psync_read_socket(conn->sock, buf, len);
  if (ret==-1) {
    err=psync_sock_err();
    /* FIXME: Condition is always false when reached */
    if (err==P_WOULDBLOCK || err==P_AGAIN || err==P_INTR)
      return MBEDTLS_ERR_SSL_WANT_READ;
    else
      return MBEDTLS_ERR_NET_RECV_FAILED;
  }
  else
    return (int)ret;
}

/*
 * TODO: It seems this is not used anymore.
 * Previously this was used in psync_ssl_connect()
 * for mbedtls_ssl_set_bio().
 */
static int psync_mbed_write(void *ptr, const unsigned char *buf, size_t len) {
  ssl_connection_t *conn;
  ssize_t ret;
  int err;
  conn=(ssl_connection_t *)ptr;
  ret=psync_write_socket(conn->sock, buf, len);
  if (ret==-1) {
    err=psync_sock_err();
    /* FIXME: Condition is always false when reached */
    if (err==P_WOULDBLOCK || err==P_AGAIN || err==P_INTR)
      return MBEDTLS_ERR_SSL_WANT_WRITE;
    else
      return MBEDTLS_ERR_NET_SEND_FAILED;
  }
  else
    return (int)ret;
}

static void psync_ssl_free_session(void *ptr) {
  mbedtls_ssl_session_free((mbedtls_ssl_session *)ptr);
  psync_free(ptr);
}


static void psync_ssl_save_session(ssl_connection_t *conn) {
  mbedtls_ssl_session *sess;
  sess=psync_new(mbedtls_ssl_session);
  /* mbedtls_ssl_get_session seems to copy all elements, instead of
   * referencing them, therefore it is thread safe to add session upon
   * connect */
  memset(sess, 0, sizeof(mbedtls_ssl_session));
  if (mbedtls_ssl_get_session(&conn->ssl, sess))
    psync_free(sess);
  else
    psync_cache_add(conn->cachekey, sess, PSYNC_SSL_SESSION_CACHE_TIMEOUT, psync_ssl_free_session, PSYNC_MAX_SSL_SESSIONS_PER_DOMAIN);
}

static int psync_ssl_check_peer_public_key(ssl_connection_t *conn) {
  const mbedtls_x509_crt *cert;
  unsigned char buff[1024], sigbin[32];
  char sighex[66];
  int i;
  cert=mbedtls_ssl_get_peer_cert(&conn->ssl);
  if (!cert) {
    log_warn("mbedtls_ssl_get_peer_cert returned NULL");
    return -1;
  }
  if (mbedtls_pk_get_type(&cert->pk)!=MBEDTLS_PK_RSA) {
    log_warn("public key is not RSA");
    return -1;
  }
  i=mbedtls_pk_write_pubkey_der((mbedtls_pk_context *)&cert->pk, buff, sizeof(buff));
  if (i<=0) {
    log_warn("pk_write_pubkey_der returned error %d", i);
    return -1;
  }
  mbedtls_sha256(buff+sizeof(buff)-i, i, sigbin, 0);
  psync_binhex(sighex, sigbin, 32);
  sighex[64]=0;
  for (i=0; i<ARRAY_SIZE(psync_ssl_trusted_pk_sha256); i++)
    if (!strcmp(sighex, psync_ssl_trusted_pk_sha256[i]))
      return 0;
  log_error(
      "got sha256hex of public key %s that does not match any approved fingerprint",
      sighex
  );
  return -1;
}

int psync_ssl_connect(psync_socket_t sock, void **sslconn, const char *hostname) {
  ssl_connection_t *conn;
  mbedtls_ssl_session *sess;
  int ret;

  conn = psync_ssl_alloc_conn(hostname);
  mbedtls_ssl_init(&conn->ssl);
  conn->sock = sock;

  /*  Setup stuff */
  mbedtls_ssl_config_init(&conn->config);
  if ((ret = mbedtls_ssl_config_defaults(
      &conn->config,
      MBEDTLS_SSL_IS_CLIENT,
      MBEDTLS_SSL_TRANSPORT_STREAM,
      MBEDTLS_SSL_PRESET_DEFAULT)) != 0)
  {
    mbedtls_log_error(ret);
    goto err1;
  }

  /* TLS 1.2 */
  mbedtls_ssl_conf_min_version(
      &conn->config,
      MBEDTLS_SSL_MAJOR_VERSION_3,
      MBEDTLS_SSL_MINOR_VERSION_3
  );

  mbedtls_ssl_conf_ca_chain(&conn->config, &psync_mbed_trusted_certs_x509, NULL);
  mbedtls_ssl_conf_ciphersuites(&conn->config, psync_mbed_ciphersuite);
  mbedtls_ssl_conf_rng(&conn->config, ctr_drbg_random_locked, &psync_mbed_rng);

  /* Setup an SSL context */
  if ((ret = mbedtls_ssl_setup(&conn->ssl, &conn->config)) != 0) {
    mbedtls_log_error(ret);
    goto err1;
  }

  /* Setup SSL hostname */
  if ((ret = mbedtls_ssl_set_hostname(&conn->ssl, hostname)) != 0) {
    mbedtls_log_error(ret);
    goto err1;
  }

  /* Set the underlying BIO callbacks for write, read and read-with-timeout */
  mbedtls_ssl_set_bio(
      &conn->ssl,
      conn,
      psync_mbed_write,
      psync_mbed_read,
      NULL
  );

  /* we do not need SNI, but should not hurt in general to support */
  mbedtls_ssl_set_hostname(&conn->ssl, hostname);

  if ((sess=(mbedtls_ssl_session *)psync_cache_get(conn->cachekey))) {
    log_info("reusing cached session for %s", hostname);
    if (mbedtls_ssl_set_session(&conn->ssl, sess))
      log_warn("ssl_set_session failed");
    mbedtls_ssl_session_free(sess);
    psync_free(sess);
  }

  /* Performing the SSL/TLS handshake */
  ret = mbedtls_ssl_handshake(&conn->ssl);

  if (ret == 0) {
    if (psync_ssl_check_peer_public_key(conn))
      goto err1;
    *sslconn=conn;
    psync_ssl_save_session(conn);
    return PSYNC_SSL_SUCCESS;
  }

  psync_set_ssl_error(conn, ret);
  if (likely(ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)) {
    *sslconn = conn;
    return PSYNC_SSL_NEED_FINISH;
  }
  mbedtls_log_error(ret);
err1:
  mbedtls_ssl_free(&conn->ssl);

  psync_free(conn);
  return PSYNC_SSL_FAIL;
}

int psync_ssl_connect_finish(void *sslconn, const char *hostname) {
  ssl_connection_t *conn;
  int ret;
  conn = (ssl_connection_t *)sslconn;
  ret = mbedtls_ssl_handshake(&conn->ssl);

  if (ret == 0) {
    if (psync_ssl_check_peer_public_key(conn))
      goto fail;
    psync_ssl_save_session(conn);
    return PSYNC_SSL_SUCCESS;
  }

  psync_set_ssl_error(conn, ret);
  if (likely(ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE))
    return PSYNC_SSL_NEED_FINISH;

  mbedtls_log_error(ret);
fail:
  mbedtls_ssl_free(&conn->ssl);
  psync_free(conn);
  return PSYNC_SSL_FAIL;
}

int psync_ssl_shutdown(void *sslconn) {
  ssl_connection_t *conn = (ssl_connection_t *)sslconn;
  if (conn->isbroken)
    goto noshutdown;

  int ret = mbedtls_ssl_close_notify(&conn->ssl);
  if (ret == 0)
    goto noshutdown;

  psync_set_ssl_error(conn, ret);
  if (likely(ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE))
    return PSYNC_SSL_NEED_FINISH;

  mbedtls_log_error(ret);
noshutdown:
  mbedtls_ssl_free(&conn->ssl);
  psync_free(conn);
  return PSYNC_SSL_SUCCESS;
}

void psync_ssl_free(void *sslconn) {
  ssl_connection_t *conn;
  conn=(ssl_connection_t *)sslconn;
  mbedtls_ssl_free(&conn->ssl);
  psync_free(conn);
}

size_t psync_ssl_pendingdata(void *sslconn) {
  return mbedtls_ssl_get_bytes_avail(&((ssl_connection_t *)sslconn)->ssl);
}

int psync_ssl_read(void *sslconn, void *buf, int num) {
  ssl_connection_t *conn = (ssl_connection_t *)sslconn;
  int ret = mbedtls_ssl_read(&conn->ssl, (unsigned char *)buf, num);
  if (ret >= 0)
    return ret;

  psync_set_ssl_error(conn, ret);
  if (ret < 0) {
    mbedtls_log_error(ret);
  }

  return PSYNC_SSL_FAIL;
}

int psync_ssl_write(void *sslconn, const void *buf, int num) {
  ssl_connection_t *conn = (ssl_connection_t *)sslconn;
  int ret = mbedtls_ssl_write(&conn->ssl, (const unsigned char *)buf, num);

  if (ret >= 0)
    return ret;

  psync_set_ssl_error(conn, ret);
  if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
    mbedtls_log_error(ret);
  }

  return PSYNC_SSL_FAIL;
}

void psync_ssl_rand_strong(unsigned char *buf, int num) {
  if (unlikely(ctr_drbg_random_locked(&psync_mbed_rng, buf, num))) {
    log_fatal("could not generate %d random bytes, exiting", num);
    abort();
  }
}

void psync_ssl_rand_weak(unsigned char *buf, int num) {
  psync_ssl_rand_strong(buf, num);
}

psync_rsa_t psync_ssl_gen_rsa(int bits) {
  mbedtls_rsa_context *ctx;
  ctx=psync_new(mbedtls_rsa_context);
  mbedtls_rsa_init(ctx, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA1);
  if (mbedtls_rsa_gen_key(ctx, ctr_drbg_random_locked, &psync_mbed_rng, bits, 65537)) {
    mbedtls_rsa_free(ctx);
    psync_free(ctx);
    return PSYNC_INVALID_RSA;
  }
  else
    return ctx;
}

void psync_ssl_free_rsa(psync_rsa_t rsa) {
  mbedtls_rsa_free(rsa);
  psync_free(rsa);
}

psync_rsa_publickey_t psync_ssl_rsa_get_public(psync_rsa_t rsa) {
  psync_binary_rsa_key_t bin;
  psync_rsa_publickey_t ret;
  bin=psync_ssl_rsa_public_to_binary(rsa);
  if (bin==PSYNC_INVALID_BIN_RSA)
    return PSYNC_INVALID_RSA;
  ret=psync_ssl_rsa_binary_to_public(bin);
  psync_ssl_rsa_free_binary(bin);
  return ret;
}

void psync_ssl_rsa_free_public(psync_rsa_publickey_t key) {
  psync_ssl_free_rsa(key);
}

psync_rsa_privatekey_t psync_ssl_rsa_get_private(psync_rsa_t rsa) {
  mbedtls_rsa_context *ctx;
  ctx=psync_new(mbedtls_rsa_context);
  mbedtls_rsa_init(ctx, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA1);
  if (unlikely(mbedtls_rsa_copy(ctx, rsa))) {
    mbedtls_rsa_free(ctx);
    psync_free(ctx);
    return PSYNC_INVALID_RSA;
  }
  else
    return ctx;
}

void psync_ssl_rsa_free_private(psync_rsa_privatekey_t key) {
  psync_ssl_free_rsa(key);
}

psync_binary_rsa_key_t psync_ssl_rsa_public_to_binary(psync_rsa_publickey_t rsa) {
  unsigned char buff[4096], *p;
  mbedtls_pk_context ctx;
  psync_binary_rsa_key_t ret;
  int len;
  mbedtls_pk_init(&ctx);
  if (mbedtls_pk_setup(&ctx, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)) || mbedtls_rsa_copy(mbedtls_pk_rsa(ctx), rsa))
    return PSYNC_INVALID_BIN_RSA;
  p=buff+sizeof(buff);
  len=mbedtls_pk_write_pubkey(&p, buff, &ctx);
  mbedtls_pk_free(&ctx);
  if (len<=0)
    return PSYNC_INVALID_BIN_RSA;
  ret=psync_locked_malloc(offsetof(psync_encrypted_data_struct_t, data)+len);
  ret->datalen=len;
  memcpy(ret->data, buff+sizeof(buff)-len, len);
  return ret;
}

psync_binary_rsa_key_t psync_ssl_rsa_private_to_binary(psync_rsa_privatekey_t rsa) {
  unsigned char buff[4096];
  mbedtls_pk_context ctx;
  psync_binary_rsa_key_t ret;
  int len;
  mbedtls_pk_init(&ctx);
  if (mbedtls_pk_setup(&ctx, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)) || mbedtls_rsa_copy(mbedtls_pk_rsa(ctx), rsa))
    return PSYNC_INVALID_BIN_RSA;
  len=mbedtls_pk_write_key_der(&ctx, buff, sizeof(buff));
  mbedtls_pk_free(&ctx);
  if (len<=0)
    return PSYNC_INVALID_BIN_RSA;
  ret=psync_locked_malloc(offsetof(psync_encrypted_data_struct_t, data)+len);
  ret->datalen=len;
  memcpy(ret->data, buff+sizeof(buff)-len, len);
  psync_ssl_memclean(buff+sizeof(buff)-len, len);
  return ret;
}

psync_rsa_publickey_t psync_ssl_rsa_load_public(const unsigned char *keydata, size_t keylen) {
  mbedtls_pk_context ctx;
  mbedtls_rsa_context *rsa;
  int ret;
  mbedtls_pk_init(&ctx);
  if (unlikely(ret=mbedtls_pk_parse_public_key(&ctx, keydata, keylen))) {
    log_warn("mbedtls_pk_parse_public_key failed with code %d", ret);
    return PSYNC_INVALID_RSA;
  }
  rsa=psync_new(mbedtls_rsa_context);
  mbedtls_rsa_init(rsa, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA1);
  ret=mbedtls_rsa_copy(rsa, mbedtls_pk_rsa(ctx));
  mbedtls_pk_free(&ctx);
  if (unlikely(ret)) {
    mbedtls_rsa_free(rsa);
    psync_free(rsa);
    return PSYNC_INVALID_RSA;
  }
  else{
    mbedtls_rsa_set_padding(rsa, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA1);
    return rsa;
  }
}

psync_rsa_privatekey_t psync_ssl_rsa_load_private(const unsigned char *keydata, size_t keylen) {
  mbedtls_pk_context ctx;
  mbedtls_rsa_context *rsa;
  int ret;
  mbedtls_pk_init(&ctx);
  if (unlikely(ret = mbedtls_pk_parse_key(&ctx, keydata, keylen, NULL, 0))) {
    mbedtls_log_error(ret);
    return PSYNC_INVALID_RSA;
  }
  rsa=psync_new(mbedtls_rsa_context);
  mbedtls_rsa_init(rsa, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA1);
  ret=mbedtls_rsa_copy(rsa, mbedtls_pk_rsa(ctx));
  mbedtls_pk_free(&ctx);
  if (unlikely(ret)) {
    mbedtls_log_error(ret);
    mbedtls_rsa_free(rsa);
    psync_free(rsa);
    return PSYNC_INVALID_RSA;
  }
  else{
    mbedtls_rsa_set_padding(rsa, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA1);
    return rsa;
  }
}

psync_rsa_publickey_t psync_ssl_rsa_binary_to_public(psync_binary_rsa_key_t bin) {
  return psync_ssl_rsa_load_public(bin->data, bin->datalen);
}

psync_rsa_privatekey_t psync_ssl_rsa_binary_to_private(psync_binary_rsa_key_t bin) {
  return psync_ssl_rsa_load_private(bin->data, bin->datalen);
}

psync_symmetric_key_t psync_ssl_gen_symmetric_key_from_pass(const char *password, size_t keylen, const unsigned char *salt, size_t saltlen, size_t iterations) {
  psync_symmetric_key_t key=(psync_symmetric_key_t)psync_locked_malloc(keylen+offsetof(psync_symmetric_key_struct_t, key));
  mbedtls_md_context_t ctx;
  /* TODO: mbedtls_md_init_ctx is deprecated */
  mbedtls_md_init_ctx(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA512));
  key->keylen=keylen;
  mbedtls_pkcs5_pbkdf2_hmac(&ctx, (const unsigned char *)password, strlen(password), salt, saltlen, iterations, keylen, key->key);
  return key;
}

char *psync_ssl_derive_password_from_passphrase(const char *username, const char *passphrase) {
  unsigned char *usercopy;
  unsigned char usersha512[PSYNC_SHA512_DIGEST_LEN], passwordbin[32];
  mbedtls_md_context_t ctx;
  size_t userlen, i;
  userlen=strlen(username);
  usercopy=psync_new_cnt(unsigned char, userlen);
  for (i=0; i<userlen; i++)
    if ((unsigned char)username[i]<=127)
      usercopy[i]=tolower((unsigned char)username[i]);
    else
      usercopy[i]='*';
  psync_sha512(usercopy, userlen, usersha512);
  psync_free(usercopy);
  mbedtls_md_init_ctx(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA512));
  mbedtls_pkcs5_pbkdf2_hmac(&ctx, (const unsigned char *)passphrase, strlen(passphrase), usersha512, PSYNC_SHA512_DIGEST_LEN, 5000, sizeof(passwordbin), passwordbin);
  usercopy=psync_base64_encode(passwordbin, sizeof(passwordbin), &userlen);
  return (char *)usercopy;
}

psync_encrypted_symmetric_key_t psync_ssl_rsa_encrypt_data(psync_rsa_publickey_t rsa, const unsigned char *data, size_t datalen) {
  psync_encrypted_symmetric_key_t ret;
  int code;
  ret=(psync_encrypted_symmetric_key_t)psync_malloc(offsetof(psync_encrypted_data_struct_t, data)+rsa->len);
  if ((code=mbedtls_rsa_rsaes_oaep_encrypt(rsa, ctr_drbg_random_locked, &psync_mbed_rng, MBEDTLS_RSA_PUBLIC, NULL, 0, datalen, data, ret->data))) {
    mbedtls_log_error(code);
    psync_free(ret);
    return PSYNC_INVALID_ENC_SYM_KEY;
  }
  ret->datalen=rsa->len;
  log_info("datalen=%lu", (unsigned long)ret->datalen);
  return ret;
}

psync_symmetric_key_t psync_ssl_rsa_decrypt_data(psync_rsa_privatekey_t rsa, const unsigned char *data, size_t datalen) {
  unsigned char buff[2048];
  psync_symmetric_key_t ret;
  size_t len;
  if (mbedtls_rsa_rsaes_oaep_decrypt(rsa, ctr_drbg_random_locked, &psync_mbed_rng, MBEDTLS_RSA_PRIVATE, NULL, 0, &len, data, buff, sizeof(buff)))
    return PSYNC_INVALID_SYM_KEY;
  ret=(psync_symmetric_key_t)psync_locked_malloc(offsetof(psync_symmetric_key_struct_t, key)+len);
  ret->keylen=len;
  memcpy(ret->key, buff, len);
  psync_ssl_memclean(buff, len);
  return ret;
}

psync_encrypted_symmetric_key_t psync_ssl_rsa_encrypt_symmetric_key(psync_rsa_publickey_t rsa, const psync_symmetric_key_t key) {
  return psync_ssl_rsa_encrypt_data(rsa, key->key, key->keylen);
}

psync_symmetric_key_t psync_ssl_rsa_decrypt_symmetric_key(psync_rsa_privatekey_t rsa, const psync_encrypted_symmetric_key_t enckey) {
  return psync_ssl_rsa_decrypt_data(rsa, enckey->data, enckey->datalen);
}

psync_aes256_encoder psync_ssl_aes256_create_encoder(psync_symmetric_key_t key) {
  mbedtls_aes_context *aes;
  assert(key->keylen>=PSYNC_AES256_KEY_SIZE);
  aes=psync_new(mbedtls_aes_context);
  mbedtls_aes_setkey_enc(aes, key->key, 256);
  return aes;
}

void psync_ssl_aes256_free_encoder(psync_aes256_encoder aes) {
  psync_ssl_memclean(aes, sizeof(mbedtls_aes_context));
  psync_free(aes);
}

psync_aes256_encoder psync_ssl_aes256_create_decoder(psync_symmetric_key_t key) {
  mbedtls_aes_context *aes;
  assert(key->keylen>=PSYNC_AES256_KEY_SIZE);
  aes=psync_new(mbedtls_aes_context);
  mbedtls_aes_setkey_dec(aes, key->key, 256);
  return aes;
}

void psync_ssl_aes256_free_decoder(psync_aes256_encoder aes) {
  psync_ssl_memclean(aes, sizeof(mbedtls_aes_context));
  psync_free(aes);
}

#ifdef PSYNC_AES_HW_GCC
#define SSE2FUNC __attribute__((__target__("sse2")))

#define AESDEC      ".byte 0x66,0x0F,0x38,0xDE,"
#define AESDECLAST  ".byte 0x66,0x0F,0x38,0xDF,"
#define AESENC      ".byte 0x66,0x0F,0x38,0xDC,"
#define AESENCLAST  ".byte 0x66,0x0F,0x38,0xDD,"

#define xmm0_xmm1   "0xC8"
#define xmm0_xmm2   "0xD0"
#define xmm0_xmm3   "0xD8"
#define xmm0_xmm4   "0xE0"
#define xmm0_xmm5   "0xE8"
#define xmm1_xmm0   "0xC1"
#define xmm1_xmm2   "0xD1"
#define xmm1_xmm3   "0xD9"
#define xmm1_xmm4   "0xE1"
#define xmm1_xmm5   "0xE9"

SSE2FUNC void psync_aes256_encode_block_hw(psync_aes256_encoder enc, const unsigned char *src, const unsigned char *dst) {
  asm("movdqu (%0), %%xmm0\n"
      "lea 16(%0), %0\n"
      "movdqa (%1), %%xmm1\n"
      "dec %3\n"
      "pxor %%xmm0, %%xmm1\n"
      "movdqu (%0), %%xmm0\n"
      "1:\n"
      "lea 16(%0), %0\n"
      "dec %3\n"
      AESENC xmm0_xmm1 "\n"
      "movdqu (%0), %%xmm0\n"
      "jnz 1b\n"
      AESENCLAST xmm0_xmm1 "\n"
      "movdqa %%xmm1, (%2)\n"
      :
      : "r" (enc->rk), "r" (src), "r" (dst),  "r" (enc->nr)
      : "memory", "cc", "xmm0", "xmm1"
  );
}

SSE2FUNC void psync_aes256_decode_block_hw(psync_aes256_decoder enc, const unsigned char *src, const unsigned char *dst) {
  asm("movdqu (%0), %%xmm0\n"
      "lea 16(%0), %0\n"
      "movdqa (%1), %%xmm1\n"
      "dec %3\n"
      "pxor %%xmm0, %%xmm1\n"
      "movdqu (%0), %%xmm0\n"
      "1:\n"
      "lea 16(%0), %0\n"
      "dec %3\n"
      AESDEC xmm0_xmm1 "\n"
      "movdqu (%0), %%xmm0\n"
      "jnz 1b\n"
      AESDECLAST xmm0_xmm1 "\n"
      "movdqa %%xmm1, (%2)\n"
      :
      : "r" (enc->rk), "r" (src), "r" (dst),  "r" (enc->nr)
      : "memory", "cc", "xmm0", "xmm1"
  );
}

SSE2FUNC void psync_aes256_encode_2blocks_consec_hw(psync_aes256_encoder enc, const unsigned char *src, const unsigned char *dst) {
  asm("movdqu (%0), %%xmm0\n"
      "movdqa (%1), %%xmm1\n"
      "dec %3\n"
      "movdqa 16(%1), %%xmm2\n"
      "lea 16(%0), %0\n"
      "xorps %%xmm0, %%xmm1\n"
      "pxor %%xmm0, %%xmm2\n"
      "movdqu (%0), %%xmm0\n"
      "1:\n"
      "lea 16(%0), %0\n"
      AESENC xmm0_xmm1 "\n"
      "dec %3\n"
      AESENC xmm0_xmm2 "\n"
      "movdqu (%0), %%xmm0\n"
      "jnz 1b\n"
      AESENCLAST xmm0_xmm1 "\n"
      AESENCLAST xmm0_xmm2 "\n"
      "movdqa %%xmm1, (%2)\n"
      "movdqa %%xmm2, 16(%2)\n"
      :
      : "r" (enc->rk), "r" (src), "r" (dst),  "r" (enc->nr)
      : "memory", "cc", "xmm0", "xmm1", "xmm2"
  );
}

SSE2FUNC void psync_aes256_decode_2blocks_consec_hw(psync_aes256_decoder enc, const unsigned char *src, const unsigned char *dst) {
  asm("movdqu (%0), %%xmm0\n"
      "movdqa (%1), %%xmm1\n"
      "dec %3\n"
      "movdqa 16(%1), %%xmm2\n"
      "lea 16(%0), %0\n"
      "xorps %%xmm0, %%xmm1\n"
      "pxor %%xmm0, %%xmm2\n"
      "movdqu (%0), %%xmm0\n"
      "1:\n"
      "lea 16(%0), %0\n"
      AESDEC xmm0_xmm1 "\n"
      "dec %3\n"
      AESDEC xmm0_xmm2 "\n"
      "movdqu (%0), %%xmm0\n"
      "jnz 1b\n"
      AESDECLAST xmm0_xmm1 "\n"
      AESDECLAST xmm0_xmm2 "\n"
      "movdqa %%xmm1, (%2)\n"
      "movdqa %%xmm2, 16(%2)\n"
      :
      : "r" (enc->rk), "r" (src), "r" (dst),  "r" (enc->nr)
      : "memory", "cc", "xmm0", "xmm1", "xmm2"
  );
}

SSE2FUNC void psync_aes256_decode_4blocks_consec_xor_hw(psync_aes256_decoder enc, const unsigned char *src, const unsigned char *dst, const unsigned char *bxor) {
  asm("movdqu (%0), %%xmm0\n"
      "shr %4\n"
      "movdqa (%1), %%xmm2\n"
      "dec %4\n"
      "movdqa 16(%1), %%xmm3\n"
      "xorps %%xmm0, %%xmm2\n"
      "movdqa 32(%1), %%xmm4\n"
      "xorps %%xmm0, %%xmm3\n"
      "movdqa 48(%1), %%xmm5\n"
      "pxor %%xmm0, %%xmm4\n"
      "movdqu 16(%0), %%xmm1\n"
      "pxor %%xmm0, %%xmm5\n"
      "1:\n"
      "lea 32(%0), %0\n"
      "dec %4\n"
      AESDEC xmm1_xmm2 "\n"
      "movdqu (%0), %%xmm0\n"
      AESDEC xmm1_xmm3 "\n"
      AESDEC xmm1_xmm4 "\n"
      AESDEC xmm1_xmm5 "\n"
      AESDEC xmm0_xmm2 "\n"
      "movdqu 16(%0), %%xmm1\n"
      AESDEC xmm0_xmm3 "\n"
      AESDEC xmm0_xmm4 "\n"
      AESDEC xmm0_xmm5 "\n"
      "jnz 1b\n"
      AESDEC xmm1_xmm2 "\n"
      "movdqu 32(%0), %%xmm0\n"
      AESDEC xmm1_xmm3 "\n"
      AESDEC xmm1_xmm4 "\n"
      AESDEC xmm1_xmm5 "\n"
      AESDECLAST xmm0_xmm2 "\n"
      AESDECLAST xmm0_xmm3 "\n"
      AESDECLAST xmm0_xmm4 "\n"
      "pxor (%3), %%xmm2\n"
      AESDECLAST xmm0_xmm5 "\n"
      "pxor 16(%3), %%xmm3\n"
      "movdqa %%xmm2, (%2)\n"
      "pxor 32(%3), %%xmm4\n"
      "movdqa %%xmm3, 16(%2)\n"
      "pxor 48(%3), %%xmm5\n"
      "movdqa %%xmm4, 32(%2)\n"
      "movdqa %%xmm5, 48(%2)\n"
      :
      : "r" (enc->rk), "r" (src), "r" (dst),  "r" (bxor), "r" (enc->nr)
      : "memory", "cc", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5"
  );
}
#elif defined(PSYNC_AES_HW_MSC)
void psync_aes256_encode_block_hw(psync_aes256_encoder enc, const unsigned char *src, const unsigned char *dst) {
  __m128i r0, r1;
  unsigned char *key;
  unsigned cnt;
  key=(unsigned char *)enc->rk;
  r0=_mm_loadu_si128((__m128i *)key);
  r1=_mm_load_si128((__m128i *)src);
  cnt=enc->nr-1;
  key+=16;
  r1=_mm_xor_si128(r0, r1);
  r0=_mm_loadu_si128((__m128i *)key);
  do{
    key+=16;
    r1=_mm_aesenc_si128(r1, r0);
    r0=_mm_loadu_si128((__m128i *)key);
  } while (--cnt);
  r1=_mm_aesenclast_si128(r1, r0);
  _mm_store_si128((__m128i *)dst, r1);
}

void void psync_aes256_decode_block_hw(psync_aes256_decoder enc, const unsigned char *src, const unsigned char *dst) {
  __m128i r0, r1;
  unsigned char *key;
  unsigned cnt;
  key=(unsigned char *)enc->rk;
  r0=_mm_loadu_si128((__m128i *)key);
  r1=_mm_load_si128((__m128i *)src);
  cnt=enc->nr-1;
  key+=16;
  r1=_mm_xor_si128(r0, r1);
  r0=_mm_loadu_si128((__m128i *)key);
  do{
    key+=16;
    r1=_mm_aesdec_si128(r1, r0);
    r0=_mm_loadu_si128((__m128i *)key);
  } while (--cnt);
  r1=_mm_aesdeclast_si128(r1, r0);
  _mm_store_si128((__m128i *)dst, r1);
}

void psync_aes256_encode_2blocks_consec_hw(psync_aes256_encoder enc, const unsigned char *src, const unsigned char *dst) {
  __m128i r0, r1, r2;
  unsigned char *key;
  unsigned cnt;
  key=(unsigned char *)enc->rk;
  r0=_mm_loadu_si128((__m128i *)key);
  r1=_mm_load_si128((__m128i *)src);
  r2=_mm_load_si128((__m128i *)(src+16));
  cnt=enc->nr-1;
  key+=16;
  r1=_mm_xor_si128(r0, r1);
  r2=_mm_xor_si128(r0, r2);
  r0=_mm_loadu_si128((__m128i *)key);
  do{
    key+=16;
    r1=_mm_aesenc_si128(r1, r0);
    r2=_mm_aesenc_si128(r2, r0);
    r0=_mm_loadu_si128((__m128i *)key);
  } while (--cnt);
  r1=_mm_aesenclast_si128(r1, r0);
  r2=_mm_aesenclast_si128(r2, r0);
  _mm_store_si128((__m128i *)dst, r1);
  _mm_store_si128((__m128i *)(dst+16), r2);
}

void psync_aes256_decode_2blocks_consec_hw(psync_aes256_decoder enc, const unsigned char *src, const unsigned char *dst) {
  __m128i r0, r1, r2;
  unsigned char *key;
  unsigned cnt;
  key=(unsigned char *)enc->rk;
  r0=_mm_loadu_si128((__m128i *)key);
  r1=_mm_load_si128((__m128i *)src);
  r2=_mm_load_si128((__m128i *)(src+16));
  cnt=enc->nr-1;
  key+=16;
  r1=_mm_xor_si128(r0, r1);
  r2=_mm_xor_si128(r0, r2);
  r0=_mm_loadu_si128((__m128i *)key);
  do{
    key+=16;
    r1=_mm_aesdec_si128(r1, r0);
    r2=_mm_aesdec_si128(r2, r0);
    r0=_mm_loadu_si128((__m128i *)key);
  } while (--cnt);
  r1=_mm_aesdeclast_si128(r1, r0);
  r2=_mm_aesdeclast_si128(r2, r0);
  _mm_store_si128((__m128i *)dst, r1);
  _mm_store_si128((__m128i *)(dst+16), r2);
}

void psync_aes256_decode_4blocks_consec_xor_hw(psync_aes256_decoder enc, const unsigned char *src, const unsigned char *dst, const unsigned char *bxor) {
  __m128i r0, r1, r2, r3, r4;
  unsigned char *key;
  unsigned cnt;
  key=(unsigned char *)enc->rk;
  r0=_mm_loadu_si128((__m128i *)key);
  r1=_mm_load_si128((__m128i *)src);
  r2=_mm_load_si128((__m128i *)(src+16));
  r3=_mm_load_si128((__m128i *)(src+32));
  r4=_mm_load_si128((__m128i *)(src+48));
  cnt=enc->nr-1;
  key+=16;
  r1=_mm_xor_si128(r0, r1);
  r2=_mm_xor_si128(r0, r2);
  r3=_mm_xor_si128(r0, r3);
  r4=_mm_xor_si128(r0, r4);
  r0=_mm_loadu_si128((__m128i *)key);
  do{
    key+=16;
    r1=_mm_aesdec_si128(r1, r0);
    r2=_mm_aesdec_si128(r2, r0);
    r3=_mm_aesdec_si128(r3, r0);
    r4=_mm_aesdec_si128(r4, r0);
    r0=_mm_loadu_si128((__m128i *)key);
  } while (--cnt);
  r1=_mm_aesdeclast_si128(r1, r0);
  r2=_mm_aesdeclast_si128(r2, r0);
  r3=_mm_aesdeclast_si128(r3, r0);
  r4=_mm_aesdeclast_si128(r4, r0);
  r0=_mm_load_si128((__m128i *)bxor);
  r1=_mm_xor_si128(r0, r1);
  r0=_mm_load_si128((__m128i *)(bxor+16));
  _mm_store_si128((__m128i *)dst, r1);
  r2=_mm_xor_si128(r0, r2);
  r0=_mm_load_si128((__m128i *)(bxor+32));
  _mm_store_si128((__m128i *)(dst+16), r2);
  r3=_mm_xor_si128(r0, r3);
  r0=_mm_load_si128((__m128i *)(bxor+48));
  _mm_store_si128((__m128i *)(dst+32), r3);
  r4=_mm_xor_si128(r0, r4);
  _mm_store_si128((__m128i *)(dst+48), r4);
}
#endif  /* PSYNC_AES_HW_GCC, PSYNC_AES_HW_MSC */

#ifdef PSYNC_AES_HW
void psync_aes256_decode_4blocks_consec_xor_sw(psync_aes256_decoder enc, const unsigned char *src, unsigned char *dst, const unsigned char *bxor) {
  unsigned long i;

  mbedtls_aes_crypt_ecb(enc, MBEDTLS_AES_DECRYPT, src, dst);
  mbedtls_aes_crypt_ecb(enc, MBEDTLS_AES_DECRYPT, src+PSYNC_AES256_BLOCK_SIZE, dst+PSYNC_AES256_BLOCK_SIZE);
  mbedtls_aes_crypt_ecb(enc, MBEDTLS_AES_DECRYPT, src+PSYNC_AES256_BLOCK_SIZE*2, dst+PSYNC_AES256_BLOCK_SIZE*2);
  mbedtls_aes_crypt_ecb(enc, MBEDTLS_AES_DECRYPT, src+PSYNC_AES256_BLOCK_SIZE*3, dst+PSYNC_AES256_BLOCK_SIZE*3);

  for (i=0; i < PSYNC_AES256_BLOCK_SIZE *4 / sizeof(unsigned long); i++)
    ((unsigned long *)dst)[i] ^= ((unsigned long *)bxor)[i];
}
#endif  /* PSYNC_AES_HW */
