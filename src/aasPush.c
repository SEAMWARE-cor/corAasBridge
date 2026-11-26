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
} push = { NULL, "", 0, false };



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
  push.lost = false;
}



// -----------------------------------------------------------------------------
//
// onDisconnect -
//
static void onDisconnect(struct mosquitto* mosqP, void* userData, int rc)
{
  (void) mosqP;
  (void) userData;

  if ((rc != 0) && (push.lost == false))
  {
    COR_W("aas: push: the connection to the MQTT broker %s:%d lost - polled only, until it is back", push.host, push.port);
    push.lost = true;
  }
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
const char* aasPushStart(const char* url)
{
  const char* hostP = (strncmp(url, "mqtt://", 7) == 0) ? &url[7] : NULL;

  if (hostP == NULL)
    return "push.url - mqtt://<host>[:<port>], the MQTT broker the AAS server publishes its events to";

  const char* colon = strchr(hostP, ':');
  int         len   = (colon != NULL) ? (int) (colon - hostP) : (int) strcspn(hostP, "/");

  if ((len == 0) || (len >= (int) sizeof(push.host)))
    return "push.url - no host";

  memcpy(push.host, hostP, len);
  push.host[len] = 0;
  push.port      = 1883;

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

  mosquitto_connect_callback_set(push.mosqP, onConnect);
  mosquitto_disconnect_callback_set(push.mosqP, onDisconnect);
  mosquitto_message_callback_set(push.mosqP, onMessage);
  mosquitto_reconnect_delay_set(push.mosqP, 1, 30, true);

  int rc = mosquitto_connect_async(push.mosqP, push.host, push.port, 30);

  if (rc != MOSQ_ERR_SUCCESS)
  {
    COR_W("aas: push: the MQTT broker %s:%d: %s - tried again", push.host, push.port, mosquitto_strerror(rc));
    push.lost = true;
  }

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
  mosquitto_loop_stop(push.mosqP, true);
  mosquitto_destroy(push.mosqP);
  push.mosqP = NULL;
  push.lost  = false;
}
