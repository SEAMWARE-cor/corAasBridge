//
// FILE            aasPush.c
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
// Push - an AAS server's change events over MQTT, opt-in - see header.
//
#include <stdbool.h>                                  // bool
#include <stdio.h>                                    // snprintf
#include <stdlib.h>                                   // malloc, free, strtol
#include <string.h>                                   // strncmp, strchr, memcpy

#include <mosquitto.h>                                // mosquitto_*

#include "corLog/corLog.h"                            // COR_E, COR_W, COR_I, COR_V
#include "corBase/corBase64UrlDecode.h"               // corBase64UrlDecode
#include "corAlloc/CorAlloc.h"                        // CorAlloc
#include "corAlloc/corAllocBufferInit.h"              // corAllocBufferInit
#include "corAlloc/corAllocBufferReset.h"             // corAllocBufferReset
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeLookup.h"                    // corTreeLookup

#include "aasModel.h"                                 // aasParse
#include "aasBridge.h"                                // aasSubmodelChanged
#include "aasPush.h"                                  // Own interface



// -----------------------------------------------------------------------------
//
// push - the MQTT client
//
static struct
{
  struct mosquitto*  mosqP;
  char               host[256];
  int                port;
  bool               lost;                            // the connection lost, said once
  bool               connected;                       // connected at least once
  char               lastError[256];                  // libmosquitto's last error (its log): the TLS failure's OpenSSL text
} push = { NULL, "", 0, false, false, "" };



// -----------------------------------------------------------------------------
//
// onLog - libmosquitto's errors kept: why a connection failed, better than its error code (a TLS handshake's
// "certificate verify failed" is MOSQ_ERR_PROTOCOL)
//
static void onLog(struct mosquitto* mosqP, void* userData, int level, const char* text)
{
  (void) mosqP;
  (void) userData;

  if ((level == MOSQ_LOG_ERR) && (text != NULL))
    snprintf(push.lastError, sizeof(push.lastError), "%s", text);
}



// -----------------------------------------------------------------------------
//
// onConnect - subscribed again on every connection: Eclipse BaSyx's and FA³ST's topics
//
static void onConnect(struct mosquitto* mosqP, void* userData, int rc)
{
  (void) userData;

  if (rc != 0)
  {
    COR_W("aas: push: the MQTT broker %s:%d refused the connection: %s", push.host, push.port, mosquitto_connack_string(rc));
    return;
  }

  mosquitto_subscribe(mosqP, NULL, "sm-repository/+/submodels/#", 0);   // BaSyx (its mqtt feature)
  mosquitto_subscribe(mosqP, NULL, "events/+", 0);                      // FA³ST (MessageBusMqtt, topicPrefix "events/")

  COR_I("aas: push: connected to the MQTT broker %s:%d", push.host, push.port);
  push.lost      = false;
  push.connected = true;
}



// -----------------------------------------------------------------------------
//
// connectionGone - no connection, or none any more: said once, why - libmosquitto's last error, else rc's text.
// A connection that fails at once (mosquitto_connect_async) and one that fails in the loop are told alike
//
static void connectionGone(int rc)
{
  if (push.lost == false)
  {
    const char* why = (push.lastError[0] != 0) ? push.lastError : mosquitto_strerror(rc);

    if (push.connected == true)
      COR_W("aas: push: the connection to the MQTT broker %s:%d lost (%s) - polled only, until it is back", push.host, push.port, why);
    else
      COR_W("aas: push: no connection to the MQTT broker %s:%d (%s) - polled only, tried again", push.host, push.port, why);
    push.lost         = true;
    push.lastError[0] = 0;
  }
}



// -----------------------------------------------------------------------------
//
// onDisconnect -
//
static void onDisconnect(struct mosquitto* mosqP, void* userData, int rc)
{
  (void) mosqP;
  (void) userData;

  if (rc != 0)
    connectionGone(rc);
}



// -----------------------------------------------------------------------------
//
// payloadSubmodel - the submodel id in an event's payload, copied into idBuf: FA³ST's { "element": { "keys":
// [ { "type": "Submodel", "value" }, ... ] } }, or a submodel itself, { "id", "submodelElements" } (BaSyx's
// sm-repository/<repo>/submodels/updated). false: none
//
static bool payloadSubmodel(const struct mosquitto_message* msgP, char* idBuf, int idLen)
{
  CorAlloc ka;
  char     kaBuf[8 * 1024];
  char*    text = malloc(msgP->payloadlen + 1);
  bool     found = false;

  if (text == NULL)
    return false;

  memcpy(text, msgP->payload, msgP->payloadlen);
  text[msgP->payloadlen] = 0;

  corAllocBufferInit(&ka, kaBuf, sizeof(kaBuf), 256 * 1024, NULL, "aas push");

  CorNode* rootP    = aasParse(&ka, text);
  CorNode* elementP = ((rootP != NULL) && (rootP->type == CorObject)) ? corTreeLookup(rootP, "element") : NULL;
  CorNode* keysP    = ((elementP != NULL) && (elementP->type == CorObject)) ? corTreeLookup(elementP, "keys") : NULL;

  if ((keysP != NULL) && (keysP->type == CorArray))
  {
    for (CorNode* keyP = keysP->value.head; keyP != NULL; keyP = keyP->next)
    {
      CorNode* typeP  = (keyP->type == CorObject) ? corTreeLookup(keyP, "type")  : NULL;
      CorNode* valueP = (keyP->type == CorObject) ? corTreeLookup(keyP, "value") : NULL;

      if ((typeP != NULL) && (typeP->type == CorString) && (strcmp(typeP->value.s, "Submodel") == 0) && (valueP != NULL) && (valueP->type == CorString))
      {
        snprintf(idBuf, idLen, "%s", valueP->value.s);
        found = true;
        break;
      }
    }
  }
  else if ((rootP != NULL) && (rootP->type == CorObject) && (corTreeLookup(rootP, "submodelElements") != NULL))
  {
    CorNode* idP = corTreeLookup(rootP, "id");

    if ((idP != NULL) && (idP->type == CorString))
    {
      snprintf(idBuf, idLen, "%s", idP->value.s);
      found = true;
    }
  }

  corAllocBufferReset(&ka, false);
  free(text);

  return found;
}



// -----------------------------------------------------------------------------
//
// onMessage - an event: the submodel it is about read at once
//
//   BaSyx   sm-repository/<repo>/submodels/<id, base64url>/submodelElements/<idShortPath>/[value/]updated, ...:
//           the id from the topic; sm-repository/<repo>/submodels/{created,updated}: from the payload
//   FA³ST   events/<Event>Message: from the payload, the element's Submodel key
//
static void onMessage(struct mosquitto* mosqP, void* userData, const struct mosquitto_message* msgP)
{
  (void) mosqP;
  (void) userData;

  const char* topic = msgP->topic;
  char        id[2048];
  bool        found = false;

  if (strncmp(topic, "sm-repository/", 14) == 0)
  {
    const char* p = strchr(topic + 14, '/');          // after <repo>

    if ((p == NULL) || (strncmp(p, "/submodels/", 11) != 0))
      return;

    p += 11;

    const char* end = strchr(p, '/');

    if (end != NULL)
    {
      int len = (int) (end - p);
      int n   = ((len * 3) / 4 + 3 < (int) sizeof(id)) ? corBase64UrlDecode(p, len, (unsigned char*) id) : -1;

      if (n > 0)
      {
        id[n] = 0;
        found = true;
      }
    }
    else if (msgP->payloadlen > 0)
      found = payloadSubmodel(msgP, id, sizeof(id));
  }
  else if ((strncmp(topic, "events/", 7) == 0) && (msgP->payloadlen > 0))
    found = payloadSubmodel(msgP, id, sizeof(id));

  if (found == false)
    return;

  int endpoints = aasSubmodelChanged(id);

  COR_V("aas: push: %s - submodel '%s', %d AssetChannel(s) read now", topic, id, endpoints);
}



// -----------------------------------------------------------------------------
//
// aasPushStart -
//
const char* aasPushStart(const AasPushConfig* cP)
{
  const char* url   = cP->url;
  bool        tls   = (strncmp(url, "mqtts://", 8) == 0);
  const char* hostP = (strncmp(url, "mqtt://", 7) == 0) ? &url[7] : (tls == true) ? &url[8] : NULL;

  if (hostP == NULL)
    return "push.url - mqtt[s]://<host>[:<port>], the MQTT broker the AAS server publishes its events to";

  if ((tls == false) && ((cP->caFile != NULL) || (cP->insecure == true) || (cP->clientCert != NULL)))
    return "push.caFile, insecure, clientCert - the url is not mqtts://";

  if ((cP->clientKey != NULL) && (cP->clientCert == NULL))
    return "push.clientKey without push.clientCert";

  if ((cP->password != NULL) && (cP->username == NULL))
    return "push.passwordEnv without push.usernameEnv";

  const char* colon = strchr(hostP, ':');
  int         len   = (colon != NULL) ? (int) (colon - hostP) : (int) strcspn(hostP, "/");

  if ((len == 0) || (len >= (int) sizeof(push.host)))
    return "push.url - no host";

  memcpy(push.host, hostP, len);
  push.host[len] = 0;
  push.port      = (tls == true) ? 8883 : 1883;

  if (colon != NULL)
  {
    char* end  = NULL;
    long  port = strtol(colon + 1, &end, 10);

    if ((port <= 0) || (port > 65535) || ((*end != 0) && (*end != '/')))
      return "push.url - the port is no number of 1-65535";

    push.port = (int) port;
  }

  mosquitto_lib_init();

  if ((push.mosqP = mosquitto_new(NULL, true, NULL)) == NULL)
    return "push: the MQTT client could not be made";

  if ((cP->username != NULL) && (mosquitto_username_pw_set(push.mosqP, cP->username, cP->password) != MOSQ_ERR_SUCCESS))
  {
    aasPushStop();
    return "push: the username could not be set";
  }

  if (tls == true)
  {
    //
    // The broker's certificate: caFile's CAs, else the system's; insecure - not verified at all (neither the
    // chain nor the host name). The plugin's own: clientCert, its key in clientKey or in the same file
    //
    const char* key = (cP->clientKey != NULL) ? cP->clientKey : cP->clientCert;
    int         rc  = mosquitto_tls_set(push.mosqP, cP->caFile, (cP->caFile == NULL) ? "/etc/ssl/certs" : NULL, cP->clientCert, key, NULL);

    if (rc != MOSQ_ERR_SUCCESS)
    {
      static char why[300];

      snprintf(why, sizeof(why), "push: TLS - %s (caFile, clientCert, clientKey: PEM files that can be read, the key not encrypted)",
               mosquitto_strerror(rc));
      aasPushStop();
      return why;
    }

    if (cP->insecure == true)
    {
      mosquitto_tls_opts_set(push.mosqP, 0, NULL, NULL);   // SSL_VERIFY_NONE
      mosquitto_tls_insecure_set(push.mosqP, true);
    }
  }

  mosquitto_log_callback_set(push.mosqP, onLog);
  mosquitto_connect_callback_set(push.mosqP, onConnect);
  mosquitto_disconnect_callback_set(push.mosqP, onDisconnect);
  mosquitto_message_callback_set(push.mosqP, onMessage);
  mosquitto_reconnect_delay_set(push.mosqP, 1, 30, true);

  int rc = mosquitto_connect_async(push.mosqP, push.host, push.port, 30);

  if (rc != MOSQ_ERR_SUCCESS)
    connectionGone(rc);                               // the loop tries again

  if (mosquitto_loop_start(push.mosqP) != MOSQ_ERR_SUCCESS)
  {
    mosquitto_destroy(push.mosqP);
    push.mosqP = NULL;
    return "push: the MQTT client's thread could not be started";
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// aasPushStop -
//
void aasPushStop(void)
{
  if (push.mosqP == NULL)
    return;

  mosquitto_disconnect(push.mosqP);
  mosquitto_loop_stop(push.mosqP, true);              // not started yet: MOSQ_ERR_INVAL, harmless
  mosquitto_destroy(push.mosqP);
  push.mosqP     = NULL;
  push.lost         = false;
  push.connected    = false;
  push.lastError[0] = 0;
}
