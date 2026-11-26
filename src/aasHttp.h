#ifndef AAS_HTTP_H_
#define AAS_HTTP_H_

//
// FILE            aasHttp.h
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// HTTP/1.1 to an AAS server (IDTA Part 2, the AAS HTTP/REST API) - plain or TLS (OpenSSL), one request
// per connection (Connection: close), a body by Content-Length, chunked or up to the close.
//
// TLS has a context of the plugin's own: the server's certificate is verified against the system's CA
// certificates, or those of caFile, and must be issued to the url's host - or, insecure, not verified;
// with clientCert the plugin shows a certificate of its own (mutual TLS). The broker's own TLS settings
// are not touched.
//
// Credentials: an Authorization header's value as given (Bearer, Basic), or an OAuth2 access token - the
// client credentials grant (RFC 6749, 4.4), fetched from the token endpoint when first needed, fetched
// again before it expires and once a request is answered 401 (that request then sent again, once). One
// token for the plugin, shared by its threads.
//
#include <stdbool.h>                                  // bool



// -----------------------------------------------------------------------------
//
// AasTls - how a server on https is trusted, and the plugin's own certificate
//
typedef struct AasTls
{
  const char*  caFile;                                // NULL: the system's CA certificates
  bool         insecure;                              // the server's certificate not verified
  const char*  clientCert;                            // NULL: none; else the PEM of the plugin's certificate (and its chain)
  const char*  clientKey;                             // its private key, PEM, not encrypted; NULL: in clientCert's file
} AasTls;



// -----------------------------------------------------------------------------
//
// AasTarget - where the server is, and how it is reached
//
typedef struct AasTarget
{
  bool   tls;
  char   host[256];
  int    port;
  char   basePath[1024];                              // the API's root on the server, "" or "/api/v3.0" - no last '/'
  char   authorization[2048];                         // "": none; else the Authorization header's value
  bool   oauth2;                                      // the Authorization header the OAuth2 token (aasOauth2Init)
  bool   verifyHost;                                  // the certificate must be issued to host
  void*  sslCtx;                                      // SSL_CTX*, NULL for plain HTTP
} AasTarget;



// -----------------------------------------------------------------------------
//
// aasTargetInit - from "http[s]://<host>[:<port>][/<base path>]". authorization (NULL: none): the value of
// an Authorization header sent with every request ("Bearer ..."). tlsP: for https. NULL: ok, else why not
//
extern const char* aasTargetInit(AasTarget* tP, const char* url, const char* authorization, const AasTls* tlsP);



// -----------------------------------------------------------------------------
//
// aasTargetsHome - the configured server: what a target of the same host, port and scheme gets (its
// Authorization, its OAuth2 token, the plugin's certificate) - any other server's target only tls's CAs and
// insecure. home is copied, tls's strings too
//
extern void aasTargetsHome(const AasTarget* homeP, const AasTls* tlsP);



// -----------------------------------------------------------------------------
//
// aasTargetOf - the target of a server, "http[s]://<host>[:<port>][/<base path>]" - one per base URL, made the
// first time it is asked for, kept until aasTargetsRelease (a pointer to it stays valid). NULL: why in whyBuf
//
extern AasTarget* aasTargetOf(const char* baseUrl, char* whyBuf, int whyLen);



// -----------------------------------------------------------------------------
//
// aasTargetsRelease - every target aasTargetOf made, and the home
//
extern void aasTargetsRelease(void);



// -----------------------------------------------------------------------------
//
// aasOauth2Init - the token endpoint ("http[s]://<host>[:<port>]/<path>") and the client's credentials
// (sent as HTTP Basic, RFC 6749 2.3.1); scope NULL: none asked for. Nothing is fetched yet. NULL: ok, else why
//
extern const char* aasOauth2Init(const char* tokenUrl, const char* clientId, const char* clientSecret, const char* scope, const AasTls* tlsP);



// -----------------------------------------------------------------------------
//
// aasOauth2Release -
//
extern void aasOauth2Release(void);



// -----------------------------------------------------------------------------
//
// aasTargetRelease -
//
extern void aasTargetRelease(AasTarget* tP);



// -----------------------------------------------------------------------------
//
// aasRequest - one request, path below the base path; the status (0: no answer), the body malloc'd in
// *bodyP (NULL: none). A target with oauth2: the token's Authorization - no token to be had, status 0
//
extern int aasRequest(AasTarget* tP, const char* method, const char* path, const char* body, char** bodyP, int timeoutMs);



// -----------------------------------------------------------------------------
//
// aasHttpError - why the calling thread's last request had no answer (status 0)
//
extern const char* aasHttpError(void);



// -----------------------------------------------------------------------------
//
// aasHttpVersion - the TLS library linked, for GET /version
//
extern const char* aasHttpVersion(void);

#endif  // AAS_HTTP_H_
