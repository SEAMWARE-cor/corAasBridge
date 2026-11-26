//
// FILE            aasHttp.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// HTTP/1.1 to an AAS server - see header.
//
#include <errno.h>                                    // errno, EAGAIN, EINTR
#include <netdb.h>                                    // getaddrinfo
#include <pthread.h>                                  // pthread_mutex_*
#include <stdbool.h>                                  // bool
#include <stdio.h>                                    // snprintf, sscanf
#include <stdlib.h>                                   // malloc, realloc, free, strtol
#include <string.h>                                   // strlen, strncasecmp, memcpy, memmove
#include <strings.h>                                  // strncasecmp
#include <sys/socket.h>                               // socket, connect, setsockopt
#include <sys/time.h>                                 // struct timeval
#include <time.h>                                     // time
#include <unistd.h>                                   // close

#include <openssl/err.h>                              // ERR_*
#include <openssl/ssl.h>                              // SSL_*
#include <openssl/x509.h>                             // X509_verify_cert_error_string

#include "corAlloc/CorAlloc.h"                        // CorAlloc
#include "corAlloc/corAllocBufferInit.h"              // corAllocBufferInit
#include "corAlloc/corAllocBufferReset.h"             // corAllocBufferReset
#include "corJson/CorJson.h"                          // CorJson
#include "corJson/corJsonCreate.h"                    // corJsonCreate
#include "corJson/corJsonParse.h"                     // corJsonParse
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeLookup.h"                    // corTreeLookup

#include "aasHttp.h"                                  // Own interface



//
// BODY_MAX - the largest response body read: one submodel, read whole
//
#define BODY_MAX  (16 * 1024 * 1024)



// -----------------------------------------------------------------------------
//
// error - why this thread's last request had no answer
//
static __thread char error[512];



// -----------------------------------------------------------------------------
//
// Conn - one connection: a socket, and its TLS session when the server is https
//
typedef struct Conn
{
  int   fd;
  SSL*  ssl;
} Conn;



// -----------------------------------------------------------------------------
//
// Reader - what has been read of a response and not yet consumed
//
typedef struct Reader
{
  Conn*  connP;
  char   buf[16 * 1024];
  int    len;
  int    pos;
} Reader;



// -----------------------------------------------------------------------------
//
// aasTargetInit -
//
const char* aasTargetInit(AasTarget* tP, const char* url, const char* authorization, const AasTls* tlsP)
{
  static const AasTls tlsDefault = { NULL, false, NULL, NULL };

  if (tlsP == NULL)
    tlsP = &tlsDefault;

  memset(tP, 0, sizeof(AasTarget));

  const char* rest;

  if (strncmp(url, "https://", 8) == 0)
  {
    tP->tls  = true;
    tP->port = 443;
    rest     = &url[8];
  }
  else if (strncmp(url, "http://", 7) == 0)
  {
    tP->port = 80;
    rest     = &url[7];
  }
  else
    return "server.url is http[s]://<host>[:<port>][/<base path>]";

  int hostLen = 0;

  while ((rest[hostLen] != 0) && (rest[hostLen] != ':') && (rest[hostLen] != '/'))
    hostLen++;

  if ((hostLen == 0) || (hostLen >= (int) sizeof(tP->host)))
    return "server.url has no host";

  memcpy(tP->host, rest, hostLen);
  tP->host[hostLen] = 0;
  rest = &rest[hostLen];

  if (*rest == ':')
  {
    tP->port = atoi(&rest[1]);
    if ((tP->port <= 0) || (tP->port > 65535))
      return "server.url has a port that is no TCP port";

    while ((*rest != 0) && (*rest != '/'))
      rest++;
  }

  //
  // The base path - where the server's API is rooted ("/api/v3.0" for some), without a last '/'
  //
  size_t baseLen = strlen(rest);

  while ((baseLen > 0) && (rest[baseLen - 1] == '/'))
    baseLen--;

  if (baseLen >= sizeof(tP->basePath))
    return "server.url has a base path that is too long";

  memcpy(tP->basePath, rest, baseLen);
  tP->basePath[baseLen] = 0;

  if (authorization != NULL)
  {
    if ((strlen(authorization) >= sizeof(tP->authorization)) || (strpbrk(authorization, "\r\n") != NULL))
      return "the Authorization header (server.authorizationEnv) is too long, or more than one line";

    strcpy(tP->authorization, authorization);
  }

  if (tP->tls == false)
    return NULL;

  SSL_CTX* ctxP = SSL_CTX_new(TLS_client_method());

  if (ctxP == NULL)
    return "no TLS context";

  if (tlsP->insecure == true)
    SSL_CTX_set_verify(ctxP, SSL_VERIFY_NONE, NULL);
  else
  {
    const char* caFile = tlsP->caFile;
    bool        loaded = (caFile != NULL) ? (SSL_CTX_load_verify_locations(ctxP, caFile, NULL) == 1) : (SSL_CTX_set_default_verify_paths(ctxP) == 1);

    if (loaded == false)
    {
      SSL_CTX_free(ctxP);
      return (caFile != NULL) ? "server.caFile is no PEM file of CA certificates" : "the system's CA certificates could not be loaded - server.caFile names them";
    }

    SSL_CTX_set_verify(ctxP, SSL_VERIFY_PEER, NULL);
    tP->verifyHost = true;
  }

  //
  // Mutual TLS: the plugin's certificate, shown when the server asks for one
  //
  if (tlsP->clientCert != NULL)
  {
    const char* keyFile = (tlsP->clientKey != NULL) ? tlsP->clientKey : tlsP->clientCert;

    if (SSL_CTX_use_certificate_chain_file(ctxP, tlsP->clientCert) != 1)
    {
      SSL_CTX_free(ctxP);
      return "server.clientCert is no PEM file of a certificate";
    }

    if ((SSL_CTX_use_PrivateKey_file(ctxP, keyFile, SSL_FILETYPE_PEM) != 1) || (SSL_CTX_check_private_key(ctxP) != 1))
    {
      SSL_CTX_free(ctxP);
      return "server.clientKey is no PEM file of the private key of server.clientCert (not encrypted)";
    }
  }

  tP->sslCtx = ctxP;
  return NULL;
}



// -----------------------------------------------------------------------------
//
// aasTargetRelease -
//
void aasTargetRelease(AasTarget* tP)
{
  if (tP->sslCtx != NULL)
    SSL_CTX_free((SSL_CTX*) tP->sslCtx);
  tP->sslCtx = NULL;
}



// -----------------------------------------------------------------------------
//
// connClose -
//
static void connClose(Conn* cP)
{
  if (cP->ssl != NULL)
  {
    SSL_shutdown(cP->ssl);
    SSL_free(cP->ssl);
    cP->ssl = NULL;
  }

  if (cP->fd >= 0)
    close(cP->fd);
  cP->fd = -1;
}



// -----------------------------------------------------------------------------
//
// connOpen - connected, and the TLS handshake done; every read and write times out after timeoutMs
//
static bool connOpen(AasTarget* tP, Conn* cP, int timeoutMs)
{
  struct addrinfo  hints;
  struct addrinfo* resP = NULL;
  char             port[16];

  cP->fd  = -1;
  cP->ssl = NULL;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family   = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  snprintf(port, sizeof(port), "%d", tP->port);

  int gai = getaddrinfo(tP->host, port, &hints, &resP);

  if (gai != 0)
  {
    snprintf(error, sizeof(error), "%.200s: %s", tP->host, gai_strerror(gai));
    return false;
  }

  struct timeval tv = { timeoutMs / 1000, (timeoutMs % 1000) * 1000 };

  for (struct addrinfo* aP = resP; (aP != NULL) && (cP->fd < 0); aP = aP->ai_next)
  {
    int fd = socket(aP->ai_family, aP->ai_socktype, aP->ai_protocol);

    if (fd < 0)
      continue;

    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(fd, aP->ai_addr, aP->ai_addrlen) == 0)
      cP->fd = fd;
    else
    {
      snprintf(error, sizeof(error), "connect: %s", strerror(errno));
      close(fd);
    }
  }

  freeaddrinfo(resP);

  if (cP->fd < 0)
    return false;

  if (tP->tls == false)
    return true;

  cP->ssl = SSL_new((SSL_CTX*) tP->sslCtx);

  if ((cP->ssl == NULL) || (SSL_set_fd(cP->ssl, cP->fd) != 1))
  {
    connClose(cP);
    return false;
  }

  SSL_set_tlsext_host_name(cP->ssl, tP->host);

  if ((tP->verifyHost == true) && (SSL_set1_host(cP->ssl, tP->host) != 1))   // the certificate must be issued to the host's name
  {
    connClose(cP);
    return false;
  }

  if (SSL_connect(cP->ssl) != 1)
  {
    long verify = SSL_get_verify_result(cP->ssl);

    if (verify != X509_V_OK)
      snprintf(error, sizeof(error), "the server's certificate: %s", X509_verify_cert_error_string(verify));
    else
      snprintf(error, sizeof(error), "TLS handshake: %s", ERR_reason_error_string(ERR_peek_last_error()) ? ERR_reason_error_string(ERR_peek_last_error()) : "failed");

    ERR_clear_error();
    connClose(cP);
    return false;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// connRead - >0 bytes read, 0 closed, -1 failed, -2 timed out (nothing read)
//
static int connRead(Conn* cP, char* buf, int size)
{
  if (cP->ssl == NULL)
  {
    ssize_t n = recv(cP->fd, buf, size, 0);

    if (n >= 0)
      return (int) n;

    return ((errno == EAGAIN) || (errno == EWOULDBLOCK) || (errno == EINTR)) ? -2 : -1;
  }

  int n = SSL_read(cP->ssl, buf, size);

  if (n > 0)
    return n;

  int e = SSL_get_error(cP->ssl, n);

  if (e == SSL_ERROR_ZERO_RETURN)
    return 0;

  if ((e == SSL_ERROR_WANT_READ) || ((e == SSL_ERROR_SYSCALL) && ((errno == EAGAIN) || (errno == EWOULDBLOCK) || (errno == EINTR))))
  {
    ERR_clear_error();
    return -2;
  }

  ERR_clear_error();
  return (n == 0) ? 0 : -1;
}



// -----------------------------------------------------------------------------
//
// connWrite - all of it; false: failed
//
static bool connWrite(Conn* cP, const char* buf, int len)
{
  while (len > 0)
  {
    int n = (cP->ssl != NULL) ? SSL_write(cP->ssl, buf, len) : (int) send(cP->fd, buf, len, MSG_NOSIGNAL);

    if (n <= 0)
    {
      ERR_clear_error();
      return false;
    }

    buf += n;
    len -= n;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// readerFill - more into the buffer; as connRead. stopP: a timeout is retried until it is set
//
static int readerFill(Reader* rP, volatile bool* stopP)
{
  if (rP->pos > 0)
  {
    memmove(rP->buf, &rP->buf[rP->pos], rP->len - rP->pos);
    rP->len -= rP->pos;
    rP->pos  = 0;
  }

  if (rP->len >= (int) sizeof(rP->buf))
    return -1;                                        // a line longer than the buffer

  for (;;)
  {
    int n = connRead(rP->connP, &rP->buf[rP->len], sizeof(rP->buf) - rP->len);

    if (n > 0)
      rP->len += n;

    if ((n == -2) && (stopP != NULL) && (*stopP == false))
      continue;

    return n;
  }
}



// -----------------------------------------------------------------------------
//
// readLine - one line, without its CRLF; its length, or -1 (closed, failed, too long)
//
static int readLine(Reader* rP, char* line, int size, volatile bool* stopP)
{
  for (;;)
  {
    for (int ix = rP->pos; ix < rP->len; ix++)
    {
      if (rP->buf[ix] != '\n')
        continue;

      int len = ix - rP->pos;

      if ((len > 0) && (rP->buf[ix - 1] == '\r'))
        len--;

      if (len >= size)
        return -1;

      memcpy(line, &rP->buf[rP->pos], len);
      line[len] = 0;
      rP->pos   = ix + 1;
      return len;
    }

    if (readerFill(rP, stopP) <= 0)
      return -1;
  }
}



// -----------------------------------------------------------------------------
//
// readBytes - up to n bytes of what is buffered, reading when nothing is; how many (0: closed, -1 failed)
//
static int readBytes(Reader* rP, char* out, int n, volatile bool* stopP)
{
  if (rP->pos == rP->len)
  {
    int r = readerFill(rP, stopP);

    if (r <= 0)
      return r;
  }

  int have = rP->len - rP->pos;
  int take = (have < n) ? have : n;

  memcpy(out, &rP->buf[rP->pos], take);
  rP->pos += take;
  return take;
}



// -----------------------------------------------------------------------------
//
// Body - a response's body, malloc'd
//
typedef struct Body
{
  char*  buf;
  int    len;
  bool   tooBig;
} Body;



// -----------------------------------------------------------------------------
//
// bodyAdd - bytes of the body
//
static void bodyAdd(Body* bP, const char* bytes, int n)
{
  if ((bP->tooBig == true) || (bP->len + n > BODY_MAX))
  {
    bP->tooBig = true;
    return;
  }

  char* b = realloc(bP->buf, bP->len + n + 1);

  if (b == NULL)
  {
    bP->tooBig = true;
    return;
  }

  memcpy(&b[bP->len], bytes, n);
  bP->buf          = b;
  bP->len         += n;
  bP->buf[bP->len] = 0;
}



// -----------------------------------------------------------------------------
//
// exchange - the request sent and its response read: the status, the body to bP
//
static int exchange(AasTarget* tP, const char* method, const char* path, const char* body, const char* contentType, const char* authorization, const char* accept, Body* bP, int timeoutMs, volatile bool* stopP)
{
  Conn conn;

  error[0] = 0;

  if (connOpen(tP, &conn, timeoutMs) == false)
    return 0;

  int   bodyLen = (body != NULL) ? (int) strlen(body) : 0;
  char  auth[4096 + 32] = "";
  char  type[256]       = "";
  char  head[8192];

  if ((authorization != NULL) && (authorization[0] != 0))
    snprintf(auth, sizeof(auth), "Authorization: %s\r\n", authorization);

  if (body != NULL)
    snprintf(type, sizeof(type), "Content-Type: %s\r\n", contentType);

  int   headLen = snprintf(head, sizeof(head),
                           "%s %s%s HTTP/1.1\r\nHost: %s\r\n%sAccept: %s\r\n%sContent-Length: %d\r\nConnection: close\r\n\r\n",
                           method, tP->basePath, path, tP->host, auth, accept, type, bodyLen);

  if ((headLen >= (int) sizeof(head)) || (connWrite(&conn, head, headLen) == false) || ((bodyLen > 0) && (connWrite(&conn, body, bodyLen) == false)))
  {
    snprintf(error, sizeof(error), "the request could not be sent");
    connClose(&conn);
    return 0;
  }

  Reader* rP = malloc(sizeof(Reader));

  if (rP == NULL)
  {
    connClose(&conn);
    return 0;
  }

  rP->connP = &conn;
  rP->len   = 0;
  rP->pos   = 0;

  char line[4096];
  int  status        = 0;
  int  contentLength = -1;
  bool chunked       = false;

  if ((readLine(rP, line, sizeof(line), stopP) < 0) || (sscanf(line, "HTTP/%*d.%*d %d", &status) != 1))
  {
    snprintf(error, sizeof(error), "no HTTP response");
    status = 0;
  }

  for (int len; (status != 0) && ((len = readLine(rP, line, sizeof(line), stopP)) > 0); )
  {
    if (strncasecmp(line, "Content-Length:", 15) == 0)
      contentLength = atoi(&line[15]);
    else if ((strncasecmp(line, "Transfer-Encoding:", 18) == 0) && (strstr(&line[18], "chunked") != NULL))
      chunked = true;
  }

  char chunk[4096];

  if ((status != 0) && (chunked == true))
  {
    for (;;)
    {
      if ((stopP != NULL) && (*stopP == true))
        break;

      if (readLine(rP, line, sizeof(line), stopP) < 0)
        break;

      long size = strtol(line, NULL, 16);

      if (size <= 0)
        break;

      while (size > 0)
      {
        int n = readBytes(rP, chunk, (size < (long) sizeof(chunk)) ? (int) size : (int) sizeof(chunk), stopP);

        if (n <= 0)
          break;

        bodyAdd(bP, chunk, n);
        size -= n;
      }

      if ((size > 0) || (readLine(rP, line, sizeof(line), stopP) < 0))
        break;
    }
  }
  else if (status != 0)
  {
    int left = contentLength;

    while ((left != 0) && ((stopP == NULL) || (*stopP == false)))
    {
      int n = readBytes(rP, chunk, ((left > 0) && (left < (int) sizeof(chunk))) ? left : (int) sizeof(chunk), stopP);

      if (n <= 0)
        break;

      bodyAdd(bP, chunk, n);
      if (left > 0)
        left -= n;
    }
  }

  free(rP);
  connClose(&conn);
  return status;
}



// -----------------------------------------------------------------------------
//
// targets - the servers' targets aasTargetOf made, by base URL; home, the configured server's
//
static struct
{
  pthread_mutex_t  mutex;
  AasTarget        home;
  AasTls           tls;                               // its strings strdup'd
  int              count;
  char*            urlV[64];
  AasTarget*       targetV[64];
} targets = { PTHREAD_MUTEX_INITIALIZER };



// -----------------------------------------------------------------------------
//
// aasTargetsHome -
//
void aasTargetsHome(const AasTarget* homeP, const AasTls* tlsP)
{
  pthread_mutex_lock(&targets.mutex);

  targets.home            = *homeP;
  targets.home.sslCtx     = NULL;                     // not ours: the host, port, scheme and credentials only
  targets.tls.caFile      = (tlsP->caFile     != NULL) ? strdup(tlsP->caFile)     : NULL;
  targets.tls.insecure    = tlsP->insecure;
  targets.tls.clientCert  = (tlsP->clientCert != NULL) ? strdup(tlsP->clientCert) : NULL;
  targets.tls.clientKey   = (tlsP->clientKey  != NULL) ? strdup(tlsP->clientKey)  : NULL;

  pthread_mutex_unlock(&targets.mutex);
}



// -----------------------------------------------------------------------------
//
// aasTargetOf -
//
AasTarget* aasTargetOf(const char* baseUrl, char* whyBuf, int whyLen)
{
  AasTarget* tP = NULL;

  pthread_mutex_lock(&targets.mutex);

  for (int ix = 0; ix < targets.count; ix++)
  {
    if (strcmp(targets.urlV[ix], baseUrl) == 0)
    {
      tP = targets.targetV[ix];
      break;
    }
  }

  if (tP == NULL)
  {
    AasTarget   probe;
    AasTls      other = { targets.tls.caFile, targets.tls.insecure, NULL, NULL };
    const char* why   = aasTargetInit(&probe, baseUrl, NULL, &other);
    bool        home  = (why == NULL) && (strcmp(probe.host, targets.home.host) == 0) && (probe.port == targets.home.port) && (probe.tls == targets.home.tls);

    //
    // The configured server's host: its credentials, and the plugin's certificate
    //
    if ((home == true) && (targets.tls.clientCert != NULL))
    {
      aasTargetRelease(&probe);
      why = aasTargetInit(&probe, baseUrl, NULL, &targets.tls);
    }

    if ((why == NULL) && (home == true))
    {
      snprintf(probe.authorization, sizeof(probe.authorization), "%s", targets.home.authorization);
      probe.oauth2 = targets.home.oauth2;
    }

    if (why != NULL)
      snprintf(whyBuf, whyLen, "%s: %s", baseUrl, why);
    else if (targets.count >= (int) (sizeof(targets.targetV) / sizeof(targets.targetV[0])))
    {
      snprintf(whyBuf, whyLen, "%s: more than %d servers", baseUrl, targets.count);
      aasTargetRelease(&probe);
    }
    else if (((tP = malloc(sizeof(AasTarget))) == NULL) || ((targets.urlV[targets.count] = strdup(baseUrl)) == NULL))
    {
      snprintf(whyBuf, whyLen, "out of memory");
      aasTargetRelease(&probe);
      free(tP);
      tP = NULL;
    }
    else
    {
      *tP = probe;
      targets.targetV[targets.count++] = tP;
    }
  }

  pthread_mutex_unlock(&targets.mutex);
  return tP;
}



// -----------------------------------------------------------------------------
//
// aasTargetsRelease -
//
void aasTargetsRelease(void)
{
  pthread_mutex_lock(&targets.mutex);

  for (int ix = 0; ix < targets.count; ix++)
  {
    aasTargetRelease(targets.targetV[ix]);
    free(targets.targetV[ix]);
    free(targets.urlV[ix]);
  }

  if (targets.tls.caFile     != NULL) free((char*) targets.tls.caFile);
  if (targets.tls.clientCert != NULL) free((char*) targets.tls.clientCert);
  if (targets.tls.clientKey  != NULL) free((char*) targets.tls.clientKey);

  memset(&targets.home, 0, sizeof(targets.home));
  memset(&targets.tls,  0, sizeof(targets.tls));
  targets.count = 0;

  pthread_mutex_unlock(&targets.mutex);
}



// -----------------------------------------------------------------------------
//
// oauth - the OAuth2 client credentials grant: the token endpoint, the request for a token, the token
//
static struct
{
  pthread_mutex_t  mutex;
  bool             on;
  AasTarget        endpoint;                          // the token endpoint, its path the base path
  char             basic[1024];                       // "Basic <base64 of id:secret>"
  char             body[1024];                        // "grant_type=client_credentials[&scope=...]"
  char             token[4096];                       // "Bearer <access_token>"; "": none
  time_t           until;                             // fetched again from then on; 0: when a request is refused
  int              generation;                        // which token: a 401 drops only the one it was sent with
} oauth = { PTHREAD_MUTEX_INITIALIZER, false };



// -----------------------------------------------------------------------------
//
// formEncode - application/x-www-form-urlencoded, into out; false: does not fit
//
static bool formEncode(const char* in, char* out, int size)
{
  static const char hex[] = "0123456789ABCDEF";
  int               o     = 0;

  for (const unsigned char* p = (const unsigned char*) in; *p != 0; p++)
  {
    if (o + 4 > size)
      return false;

    if (((*p >= 'A') && (*p <= 'Z')) || ((*p >= 'a') && (*p <= 'z')) || ((*p >= '0') && (*p <= '9')) || (strchr("-._~", *p) != NULL))
      out[o++] = *p;
    else if (*p == ' ')
      out[o++] = '+';
    else
    {
      out[o++] = '%';
      out[o++] = hex[*p >> 4];
      out[o++] = hex[*p & 0xF];
    }
  }

  out[o] = 0;
  return true;
}



// -----------------------------------------------------------------------------
//
// base64 - standard alphabet, padded, into out; false: does not fit
//
static bool base64(const char* in, char* out, int size)
{
  static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const unsigned char* p   = (const unsigned char*) in;
  int                  len = strlen(in);

  if ((len + 2) / 3 * 4 + 1 > size)
    return false;

  int o = 0;

  for (int ix = 0; ix < len; ix += 3)
  {
    unsigned int n = p[ix] << 16;

    if (ix + 1 < len) n |= p[ix + 1] << 8;
    if (ix + 2 < len) n |= p[ix + 2];

    out[o++] = alphabet[(n >> 18) & 63];
    out[o++] = alphabet[(n >> 12) & 63];
    out[o++] = (ix + 1 < len) ? alphabet[(n >> 6) & 63] : '=';
    out[o++] = (ix + 2 < len) ? alphabet[n & 63] : '=';
  }

  out[o] = 0;
  return true;
}



// -----------------------------------------------------------------------------
//
// aasOauth2Init -
//
const char* aasOauth2Init(const char* tokenUrl, const char* clientId, const char* clientSecret, const char* scope, const AasTls* tlsP)
{
  char id[256];
  char secret[512];
  char pair[1024];

  const char* why = aasTargetInit(&oauth.endpoint, tokenUrl, NULL, tlsP);

  if (why != NULL)
    return (strncmp(why, "server.url", 10) == 0) ? "server.oauth2.tokenUrl is http[s]://<host>[:<port>]/<path>" : why;

  if ((formEncode(clientId, id, sizeof(id)) == false) || (formEncode(clientSecret, secret, sizeof(secret)) == false))
  {
    aasTargetRelease(&oauth.endpoint);
    return "server.oauth2: the client id or its secret is too long";
  }

  snprintf(pair, sizeof(pair), "%s:%s", id, secret);
  strcpy(oauth.basic, "Basic ");

  if (base64(pair, &oauth.basic[6], sizeof(oauth.basic) - 6) == false)
  {
    aasTargetRelease(&oauth.endpoint);
    return "server.oauth2: the client id or its secret is too long";
  }

  strcpy(oauth.body, "grant_type=client_credentials");

  if (scope != NULL)
  {
    char encoded[900];

    if (formEncode(scope, encoded, sizeof(encoded)) == false)
    {
      aasTargetRelease(&oauth.endpoint);
      return "server.oauth2.scope is too long";
    }

    snprintf(&oauth.body[strlen(oauth.body)], sizeof(oauth.body) - strlen(oauth.body), "&scope=%s", encoded);
  }

  oauth.token[0]   = 0;
  oauth.until      = 0;
  oauth.generation = 0;
  oauth.on         = true;
  return NULL;
}



// -----------------------------------------------------------------------------
//
// aasOauth2Release -
//
void aasOauth2Release(void)
{
  pthread_mutex_lock(&oauth.mutex);

  if (oauth.on == true)
    aasTargetRelease(&oauth.endpoint);

  memset(oauth.basic, 0, sizeof(oauth.basic));
  memset(oauth.token, 0, sizeof(oauth.token));
  oauth.on = false;

  pthread_mutex_unlock(&oauth.mutex);
}



// -----------------------------------------------------------------------------
//
// tokenFetch - a token from the endpoint (the mutex held): { "access_token", "token_type", "expires_in" }
//
static bool tokenFetch(int timeoutMs)
{
  Body* bP = calloc(1, sizeof(Body));

  if (bP == NULL)
    return false;

  int status = exchange(&oauth.endpoint, "POST", "", oauth.body, "application/x-www-form-urlencoded", oauth.basic, "application/json", bP, timeoutMs, NULL);

  if (status == 0)
  {
    char why[400];

    snprintf(why, sizeof(why), "%.399s", error);
    snprintf(error, sizeof(error), "the OAuth2 token endpoint: %.400s", (why[0] != 0) ? why : "no answer");
    free(bP->buf);
    free(bP);
    return false;
  }

  CorAlloc    ka;
  CorJson     cj;
  char        kaBuf[8 * 1024];
  CorNode*    rootP   = NULL;
  bool        ok      = false;

  corAllocBufferInit(&ka, kaBuf, sizeof(kaBuf), 16 * 1024, NULL, "aas oauth2");
  corJsonCreate(&cj, &ka);

  if ((status == 200) && (bP->tooBig == false) && (bP->buf != NULL))
    rootP = corJsonParse(&cj, bP->buf);

  CorNode* tokenP   = ((rootP != NULL) && (rootP->type == CorObject)) ? corTreeLookup(rootP, "access_token") : NULL;
  CorNode* expiresP = ((rootP != NULL) && (rootP->type == CorObject)) ? corTreeLookup(rootP, "expires_in")   : NULL;

  if ((tokenP == NULL) || (tokenP->type != CorString) || (tokenP->value.s[0] == 0))
    snprintf(error, sizeof(error), "the OAuth2 token endpoint answered %d, %s", status, (status == 200) ? "no access_token" : "no token");
  else if ((strlen(tokenP->value.s) + 8 > sizeof(oauth.token)) || (strpbrk(tokenP->value.s, "\r\n") != NULL))
    snprintf(error, sizeof(error), "the OAuth2 token endpoint gave an access_token that is too long, or more than one line");
  else
  {
    //
    // The token is sent as a bearer token (RFC 6750) - the only token_type the grant gives in practice.
    // Fetched again a fifth of its life (at most 30 s) before it expires; no expires_in: when refused
    //
    long expires = 0;

    if ((expiresP != NULL) && (expiresP->type == CorInt))
      expires = (long) expiresP->value.i;
    else if ((expiresP != NULL) && (expiresP->type == CorFloat))
      expires = (long) expiresP->value.f;
    else if ((expiresP != NULL) && (expiresP->type == CorString))
      expires = atol(expiresP->value.s);

    snprintf(oauth.token, sizeof(oauth.token), "Bearer %s", tokenP->value.s);
    oauth.until = (expires > 0) ? time(NULL) + expires - ((expires / 5 < 30) ? expires / 5 : 30) : 0;
    oauth.generation++;
    ok = true;
  }

  corAllocBufferReset(&ka, false);
  free(bP->buf);
  free(bP);
  return ok;
}



// -----------------------------------------------------------------------------
//
// tokenGet - the Authorization for a request, and which token it is; fetched when there is none, or it is
// about to expire. false: no token to be had (why in error)
//
static bool tokenGet(char* out, int size, int* generationP, int timeoutMs)
{
  bool ok = true;

  pthread_mutex_lock(&oauth.mutex);

  if (oauth.on == false)
  {
    snprintf(error, sizeof(error), "no OAuth2 token endpoint");
    ok = false;
  }
  else if ((oauth.token[0] == 0) || ((oauth.until != 0) && (time(NULL) >= oauth.until)))
    ok = tokenFetch(timeoutMs);

  if (ok == true)
  {
    snprintf(out, size, "%s", oauth.token);
    *generationP = oauth.generation;
  }

  pthread_mutex_unlock(&oauth.mutex);
  return ok;
}



// -----------------------------------------------------------------------------
//
// tokenDrop - a request with token 'generation' answered 401: that token no longer used (a newer one stays)
//
static void tokenDrop(int generation)
{
  pthread_mutex_lock(&oauth.mutex);

  if (oauth.generation == generation)
    oauth.token[0] = 0;

  pthread_mutex_unlock(&oauth.mutex);
}



// -----------------------------------------------------------------------------
//
// aasRequest -
//
int aasRequest(AasTarget* tP, const char* method, const char* path, const char* body, char** bodyP, int timeoutMs)
{
  *bodyP = NULL;

  for (int attempt = 0; attempt < 2; attempt++)
  {
    char        bearer[sizeof(oauth.token)];
    int         generation    = 0;
    const char* authorization = tP->authorization;

    if (tP->oauth2 == true)
    {
      if (tokenGet(bearer, sizeof(bearer), &generation, timeoutMs) == false)
        return 0;

      authorization = bearer;
    }

    Body* bP = calloc(1, sizeof(Body));

    if (bP == NULL)
      return 0;

    int status = exchange(tP, method, path, body, "application/json", authorization, "application/json", bP, timeoutMs, NULL);

    //
    // 401 with the token: it is no longer taken (revoked, expired early) - a new one, the request once more
    //
    if ((status == 401) && (tP->oauth2 == true) && (attempt == 0))
    {
      free(bP->buf);
      free(bP);
      tokenDrop(generation);
      continue;
    }

    if ((bP->tooBig == false) && (bP->buf != NULL))
      *bodyP = bP->buf;
    else
      free(bP->buf);

    free(bP);
    return status;
  }

  return 0;
}



// -----------------------------------------------------------------------------
//
// aasHttpError -
//
const char* aasHttpError(void)
{
  return (error[0] != 0) ? error : "no answer";
}



// -----------------------------------------------------------------------------
//
// aasHttpVersion -
//
const char* aasHttpVersion(void)
{
  return OpenSSL_version(OPENSSL_VERSION);
}
