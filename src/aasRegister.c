//
// FILE            aasRegister.c
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
//
// The aas bridge - the SEAM: what the broker calls, and the one symbol it looks for (bridgeRegister).
// The contract is corBridge's BridgeDriver.h (broker -> plugin) and BridgeBroker.h (plugin -> broker).
//
// An Asset Administration Shell server (the AAS HTTP/REST API, IDTA Part 2) as AssetChannels: an
// AssetChannel's endpoint is a shell's id, or a submodel's - its model the shell, its submodels and their
// elements (aasModel.h), read again every pollInterval (aasBridge.h). With "channelDirection": "both" a
// written attribute is the element's new $value. An Operation is a service the bridge executes
// (aas+http[s]://...). A Channel or a DataChannel is refused - this bridge carries AssetChannels only.
//
#include <stdbool.h>                                  // bool
#include <stdint.h>                                   // int64_t
#include <stdio.h>                                    // fopen, fread, fseek, ftell, snprintf
#include <stdlib.h>                                   // malloc, free, getenv, strtod
#include <string.h>                                   // strcmp, strncmp

#include "corLog/corLog.h"                            // COR_E, COR_W
#include "corAlloc/CorAlloc.h"                        // CorAlloc
#include "corAlloc/corAllocBufferInit.h"              // corAllocBufferInit
#include "corAlloc/corAllocBufferReset.h"             // corAllocBufferReset
#include "corJson/CorJson.h"                          // CorJson
#include "corJson/corJsonCreate.h"                    // corJsonCreate
#include "corJson/corJsonParse.h"                     // corJsonParse
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeLookup.h"                    // corTreeLookup

#include "corBridge/BridgeDriver.h"                   // BridgeDriver, BridgeChannelKind, BridgeDirection
#include "corBridge/BridgeBroker.h"                   // BridgeBroker, BRIDGE_*

#include "aasHttp.h"                                  // AasTarget, AasTls, aasTargetInit, aasOauth2*, aasHttpVersion
#include "aasBridge.h"                                // aasBridge*, aasEndpoint*, aasPublish, ...
#include "aasPush.h"                                  // aasPushStart, aasPushStop



//
// POLL_MS_DEFAULT - a model read again every 10 s, unless the channel's pollInterval says otherwise
// POLL_MS_MIN     - and never more often than every 100 ms
//
#define POLL_MS_DEFAULT  10000
#define POLL_MS_MIN      100



static const BridgeBroker* hostP = NULL;



// -----------------------------------------------------------------------------
//
// configLoad - the whole --bridgeConfig file, NUL-terminated; malloc'd. NULL: not readable
//
static char* configLoad(const char* path)
{
  FILE* fP = fopen(path, "r");
  if (fP == NULL)
    return NULL;

  char* text = NULL;
  long  size = (fseek(fP, 0, SEEK_END) == 0) ? ftell(fP) : -1;

  if ((size >= 0) && (fseek(fP, 0, SEEK_SET) == 0) && ((text = malloc(size + 1)) != NULL))
  {
    if (fread(text, 1, size, fP) == (size_t) size)
      text[size] = 0;
    else
    {
      free(text);
      text = NULL;
    }
  }

  fclose(fP);
  return text;
}



// -----------------------------------------------------------------------------
//
// stringOf - a string member of objectP, NULL if none (or not a string)
//
static const char* stringOf(CorNode* objectP, const char* name)
{
  CorNode* nodeP = corTreeLookup(objectP, name);

  return ((nodeP != NULL) && (nodeP->type == CorString)) ? nodeP->value.s : NULL;
}



// -----------------------------------------------------------------------------
//
// aasInit - the server, from the --bridgeConfig file's member of this plugin:
//
//   { "aas": { "server": { "url": "https://aas.acme.example/api/v3.0", "authorizationEnv": "AAS_AUTHORIZATION",
//                          "caFile": "/etc/aas/ca.pem" },
//              "registry": { "url": "https://registry.acme.example/api/v3.0" },
//              "push": { "url": "mqtts://mqtt.acme.example", "usernameEnv": "AAS_MQTT_USER", "passwordEnv": "AAS_MQTT_PASSWORD",
//                        "caFile": "/etc/aas/mqtt-ca.pem" },
//              "ngsild": { "assetChannels": { "<shell id>": { "entities": "natural", ... } } } } }
//
// "authorizationEnv" names the environment variable that holds the Authorization header's value ("Bearer ...")
// - a credential is never in the file. Or "oauth2": { "tokenUrl", "clientId", "clientSecretEnv", "scope" } -
// an access token by the client credentials grant, the secret in the environment variable clientSecretEnv
// names. No "caFile": the system's CA certificates; "insecure": true, the server's certificate is not
// verified. "clientCert" (and "clientKey", unless the key is in clientCert's file): the plugin's own
// certificate, for a server that asks for one (mutual TLS) - the token endpoint is reached with the same.
// "registry": an AAS registry (IDTA-01002's /shell-descriptors) - where a shell and its submodels are read
// from. The credentials and the certificate go only to server.url's host (scheme, host and port): to the
// registry, and to a server it names, only if it is that host. "push": the MQTT broker the server publishes its
// change events to (BaSyx's, FA³ST's) - each event about a submodel makes its AssetChannels read at once. Its
// own credentials, from the environment (usernameEnv, passwordEnv) and its own TLS (mqtts: caFile, insecure,
// clientCert, clientKey) - the AAS server's never go to it.
//
static int aasInit(const char* configFile, const BridgeBroker* brokerP)
{
  CorAlloc    ka;
  CorJson     cj;
  char        buf[16 * 1024];
  CorNode*    aasP     = NULL;
  char*       text     = (configFile != NULL) ? configLoad(configFile) : NULL;
  const char* why      = NULL;
  const char* auth     = NULL;
  bool        oauth2   = false;
  AasTls      tls      = { NULL, false, NULL, NULL };
  AasTarget   target;
  AasTarget*  registryP = NULL;
  AasPushConfig push    = { NULL, NULL, NULL, NULL, false, NULL, NULL, NULL };

  corAllocBufferInit(&ka, buf, sizeof(buf), 16 * 1024, NULL, "aas config");
  corJsonCreate(&cj, &ka);

  if (text != NULL)
  {
    CorNode* rootP = corJsonParse(&cj, text);         // valid JSON: the broker has parsed the same file

    aasP = (rootP != NULL) ? corTreeLookup(rootP, "aas") : NULL;
  }

  CorNode* serverP = (aasP != NULL) ? corTreeLookup(aasP, "server") : NULL;

  if ((serverP == NULL) || (serverP->type != CorObject))
    why = "no \"aas\": { \"server\": { \"url\" } } in --bridgeConfig";
  else
  {
    CorNode*    insecureP = corTreeLookup(serverP, "insecure");
    CorNode*    oauthP    = corTreeLookup(serverP, "oauth2");
    const char* url       = stringOf(serverP, "url");
    const char* authEnv   = stringOf(serverP, "authorizationEnv");

    tls.insecure   = (insecureP != NULL) && (insecureP->type == CorBoolean) && (insecureP->value.b == true);
    tls.caFile     = stringOf(serverP, "caFile");
    tls.clientCert = stringOf(serverP, "clientCert");
    tls.clientKey  = stringOf(serverP, "clientKey");

    if (authEnv != NULL)
      auth = getenv(authEnv);

    if (url == NULL)
      why = "server.url - http[s]://<host>[:<port>][/<base path>], where the AAS API is";
    else if ((authEnv != NULL) && (auth == NULL))
      why = "server.authorizationEnv names an environment variable that is not set";
    else if ((authEnv != NULL) && (oauthP != NULL))
      why = "server.authorizationEnv and server.oauth2 - one of them";
    else if ((tls.clientKey != NULL) && (tls.clientCert == NULL))
      why = "server.clientKey without server.clientCert";
    else if ((tls.clientCert != NULL) && (strncmp(url, "https://", 8) != 0))
      why = "server.clientCert - the url is not https";
    else if ((oauthP != NULL) && (oauthP->type != CorObject))
      why = "server.oauth2 - { \"tokenUrl\", \"clientId\", \"clientSecretEnv\", \"scope\" }";
    else if (oauthP != NULL)
    {
      const char* tokenUrl  = stringOf(oauthP, "tokenUrl");
      const char* clientId  = stringOf(oauthP, "clientId");
      const char* secretEnv = stringOf(oauthP, "clientSecretEnv");
      const char* secret    = (secretEnv != NULL) ? getenv(secretEnv) : NULL;

      if ((tokenUrl == NULL) || (clientId == NULL) || (secretEnv == NULL))
        why = "server.oauth2 needs tokenUrl, clientId and clientSecretEnv";
      else if (secret == NULL)
        why = "server.oauth2.clientSecretEnv names an environment variable that is not set";
      else
        why = aasOauth2Init(tokenUrl, clientId, secret, stringOf(oauthP, "scope"), &tls);

      oauth2 = (why == NULL);
    }

    if (why == NULL)
    {
      why = aasTargetInit(&target, url, auth, &tls);
      target.oauth2 = oauth2;
    }

    CorNode* registryNodeP = corTreeLookup(aasP, "registry");
    CorNode* pushNodeP     = corTreeLookup(aasP, "push");

    if ((why == NULL) && (pushNodeP != NULL) && (((pushNodeP->type != CorObject) || ((push.url = stringOf(pushNodeP, "url")) == NULL))))
      why = "push - { \"url\": \"mqtt[s]://<host>[:<port>]\" }, the MQTT broker the AAS server publishes its events to";
    else if ((why == NULL) && (pushNodeP != NULL))
    {
      CorNode*    pInsecureP  = corTreeLookup(pushNodeP, "insecure");
      const char* userEnv     = stringOf(pushNodeP, "usernameEnv");
      const char* passwordEnv = stringOf(pushNodeP, "passwordEnv");

      push.username   = (userEnv     != NULL) ? getenv(userEnv)     : NULL;
      push.password   = (passwordEnv != NULL) ? getenv(passwordEnv) : NULL;
      push.caFile     = stringOf(pushNodeP, "caFile");
      push.insecure   = (pInsecureP != NULL) && (pInsecureP->type == CorBoolean) && (pInsecureP->value.b == true);
      push.clientCert = stringOf(pushNodeP, "clientCert");
      push.clientKey  = stringOf(pushNodeP, "clientKey");
      push.clientId   = stringOf(pushNodeP, "clientId");

      if ((userEnv != NULL) && (push.username == NULL))
        why = "push.usernameEnv names an environment variable that is not set";
      else if ((passwordEnv != NULL) && (push.password == NULL))
        why = "push.passwordEnv names an environment variable that is not set";
    }

    if ((why == NULL) && (registryNodeP != NULL))
    {
      const char* registryUrl = (registryNodeP->type == CorObject) ? stringOf(registryNodeP, "url") : NULL;
      static char registryWhy[600];

      aasTargetsHome(&target, &tls);

      if (registryUrl == NULL)
        why = "registry - { \"url\": \"http[s]://<host>[:<port>][/<base path>]\" }, where the AAS registry is";
      else if ((registryP = aasTargetOf(registryUrl, registryWhy, sizeof(registryWhy))) == NULL)
        why = registryWhy;
    }
  }

  if (why == NULL)
  {
    hostP = brokerP;

    if (aasBridgeOpen(&target, registryP, auth, &tls, brokerP) != BRIDGE_OK)
    {
      aasTargetRelease(&target);
      why = "the plugin's threads could not be started";
    }
    else if (push.url != NULL)
    {
      if ((why = aasPushStart(&push)) != NULL)
        aasBridgeClose();
    }
  }

  if ((why != NULL) && (oauth2 == true))
    aasOauth2Release();

  if (why != NULL)
    aasTargetsRelease();

  corAllocBufferReset(&ka, false);                    // nothing parsed here is kept: the bridge has copies
  free(text);

  if (why != NULL)
  {
    COR_E("aas: %s", why);
    return BRIDGE_BAD_INPUT;
  }

  return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// aasClose - must not return until no thread of the plugin can call the broker
//
static void aasClose(void)
{
  aasPushStop();                                      // no event after: it reads the bridge's endpoints
  aasBridgeClose();
  aasTargetsRelease();
  aasOauth2Release();
  hostP = NULL;
}



// -----------------------------------------------------------------------------
//
// durationMs - an ISO 8601 duration of hours, minutes and seconds ("PT5S", "PT0.5S", "PT1M30S") in ms; -1: not one
//
static int64_t durationMs(const char* s)
{
  if ((s[0] != 'P') || (s[1] != 'T') || (s[2] == 0))
    return -1;

  double ms   = 0;
  char   last = 0;

  for (s = &s[2]; *s != 0; )
  {
    char*  end;
    double n = strtod(s, &end);

    if ((end == s) || (n < 0))
      return -1;

    char unit = *end;

    if      ((unit == 'H') && (last == 0))                   ms += n * 3600000;
    else if ((unit == 'M') && ((last == 0) || (last == 'H'))) ms += n * 60000;
    else if ((unit == 'S') && (last != 'S'))                 ms += n * 1000;
    else
      return -1;

    last = unit;
    s    = end + 1;
  }

  return (int64_t) ms;
}



// -----------------------------------------------------------------------------
//
// pollOf - the channel's pollInterval, from channelInfo - JSON text, [ { "key", "value" } ], or NULL. A key
// this bridge does not take is refused (*whyP)
//
static int64_t pollOf(const char* info, const char** whyP)
{
  if (info == NULL)
    return POLL_MS_DEFAULT;

  CorAlloc ka;
  CorJson  cj;
  char     buf[4096];
  int64_t  pollMs = POLL_MS_DEFAULT;

  corAllocBufferInit(&ka, buf, sizeof(buf), 8 * 1024, NULL, "aas channelInfo");
  corJsonCreate(&cj, &ka);

  int   len  = strlen(info);
  char* copy = malloc(len + 1);

  if (copy == NULL)
  {
    *whyP = "out of memory";
    return -1;
  }

  memcpy(copy, info, len + 1);

  CorNode* listP = corJsonParse(&cj, copy);

  for (CorNode* itemP = ((listP != NULL) && (listP->type == CorArray)) ? listP->value.head : NULL; itemP != NULL; itemP = itemP->next)
  {
    const char* key   = (itemP->type == CorObject) ? stringOf(itemP, "key")   : NULL;
    const char* value = (itemP->type == CorObject) ? stringOf(itemP, "value") : NULL;

    if ((key != NULL) && (strcmp(key, "pollInterval") == 0))
    {
      pollMs = (value != NULL) ? durationMs(value) : -1;

      if (pollMs < POLL_MS_MIN)
      {
        *whyP  = "channelInfo pollInterval is an ISO 8601 duration of at least 0.1 s - PT5S, PT0.5S, PT1M";
        pollMs = -1;
        break;
      }
    }
    else
    {
      *whyP  = "the aas bridge takes one channelInfo key: pollInterval";
      pollMs = -1;
      break;
    }
  }

  corAllocBufferReset(&ka, false);
  free(copy);

  return pollMs;
}



// -----------------------------------------------------------------------------
//
// refused - say on the channel why it is not carried (ABI 12)
//
static void refused(const char* endpoint, const char* why)
{
#if BRIDGE_ABI_VERSION >= 12
  if ((hostP != NULL) && (hostP->abiVersion >= 12) && (hostP->channelStatusIn != NULL))
    hostP->channelStatusIn("aas", endpoint, BridgeChannelDormant, why);
#endif

  COR_W("aas: '%s' refused - %s", endpoint, why);
}



// -----------------------------------------------------------------------------
//
// The AssetChannels - each a shell's or a submodel's id. A broker of ABI 15 says which channel type an
// endpoint is (channelTypedAdd); an older one hands every channel over alike (channelAddInfo), and each is
// taken as an AssetChannel.
//
static int aasChannelAddInfo(const char* endpoint, BridgeChannelKind kind, BridgeDirection direction, const char* info)
{
  const char* why = NULL;

  if (kind != BridgeChannelTopic)
  {
    refused(endpoint, "the aas bridge carries a model's values - no service or action channel");
    return BRIDGE_UNSUPPORTED;
  }

  int64_t pollMs = pollOf(info, &why);

  if (pollMs < 0)
  {
    refused(endpoint, why);
    return BRIDGE_BAD_INPUT;
  }

  return aasEndpointAdd(endpoint, direction, pollMs);
}

#if BRIDGE_ABI_VERSION >= 15
static int aasChannelTypedAdd(const char* endpoint, BridgeChannelType type, BridgeChannelKind kind, BridgeDirection direction, const char* info)
{
  if (type == BridgeChannelTypeAssetChannel)
    return aasChannelAddInfo(endpoint, kind, direction, info);

  refused(endpoint, "the aas bridge carries AssetChannels only - an endpoint is a shell's id, or a submodel's");
  return BRIDGE_UNSUPPORTED;
}
#endif

static int aasChannelAdd(const char* endpoint, BridgeChannelKind kind, BridgeDirection direction)
{
  return aasChannelAddInfo(endpoint, kind, direction, NULL);
}

static int aasChannelDel(const char* endpoint)
{
  return aasEndpointDel(endpoint);
}

static int aasPublishOut(const char* endpoint, const char* json)
{
  return aasPublish(endpoint, json);
}



#if BRIDGE_ABI_VERSION >= 11
// -----------------------------------------------------------------------------
//
// aasServiceCancel - an invocation the server is running is not taken back
//
static int aasServiceCancel(const char* url, const char* executionId)
{
  (void) url;
  (void) executionId;

  return BRIDGE_UNSUPPORTED;
}
#endif



// -----------------------------------------------------------------------------
//
// aasVersionInfo - one line for GET /version: the plugin, and what it links
//
static const char* aasVersionInfo(void)
{
  static char info[128];

  snprintf(info, sizeof(info), "aas 0.1.0 (%s)", aasHttpVersion());
  return info;
}



// -----------------------------------------------------------------------------
//
// bridgeRegister - the one symbol the broker looks for, and the one this .so exports
//
// A slot is filled only when the broker's abiVersion has it; abiVersion is then overwritten with this
// plugin's.
//
__attribute__((visibility("default"))) void bridgeRegister(BridgeDriver* driverP)
{
  if (driverP->abiVersion < 1)
    driverP->abiVersion = 1;                          // a host from before the handshake

  driverP->alias       = "aas";                       // --bridges aas, and its member of --bridgeConfig
  driverP->version     = "0.1.0";
  driverP->args        = NULL;
  driverP->init        = aasInit;
  driverP->close       = aasClose;
  driverP->versionInfo = aasVersionInfo;
  driverP->channelAdd  = aasChannelAdd;
  driverP->channelDel  = aasChannelDel;
  driverP->publish     = aasPublishOut;

  if (driverP->abiVersion >= 9)
    driverP->channelAddInfo = aasChannelAddInfo;

#if BRIDGE_ABI_VERSION >= 11
  if (driverP->abiVersion >= 11)
  {
    driverP->serviceSchemes = "aas+http,aas+https";   // aas+http[s]://<server>/submodels/<id>/submodel-elements/<path> - an Operation
    driverP->serviceExecute = aasServiceExecute;
    driverP->serviceCancel  = aasServiceCancel;
  }
#endif

#if BRIDGE_ABI_VERSION >= 13
  if (driverP->abiVersion >= 13)
    driverP->payloadCheck = aasPayloadCheck;          // a value that does not fit the element's valueType: 400
#endif

#if BRIDGE_ABI_VERSION >= 14
  if (driverP->abiVersion >= 14)
    driverP->inventory = aasInventory;                // for a mapping tool: the server's shells and submodels
#endif

#if BRIDGE_ABI_VERSION >= 15
  if (driverP->abiVersion >= 15)
    driverP->channelTypedAdd = aasChannelTypedAdd;
#endif

  driverP->abiVersion  = BRIDGE_ABI_VERSION;
}
