#ifndef AAS_PUSH_H_
#define AAS_PUSH_H_

//
// FILE            aasPush.h
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Push, opt-in: the change events an AAS server publishes to an MQTT broker, subscribed to - each event
// about a submodel makes the AssetChannels that have it read at once, instead of at their next poll. Polling
// goes on: an event missed, or the connection lost, costs at most a pollInterval.
//
// Two servers' events: Eclipse BaSyx's (its mqtt feature, sm-repository/<repo>/submodels/...) and FA³ST
// Service's (MessageBusMqtt, events/<Event>Message, its default topicPrefix). libmosquitto, its own thread;
// the connection retried (1 s to 30 s). mqtts: TLS - the broker's certificate verified against caFile (else the
// system's CAs) unless insecure, clientCert/clientKey the plugin's own; a username and password if given.
//
#include <stdbool.h>                                  // bool



// -----------------------------------------------------------------------------
//
// AasPushConfig - the "push" member: url mqtt[s]://<host>[:<port>], the rest optional (NULL, false)
//
typedef struct AasPushConfig
{
  const char*  url;
  const char*  username;
  const char*  password;
  const char*  caFile;
  bool         insecure;
  const char*  clientCert;
  const char*  clientKey;                             // NULL: in clientCert's file
  const char*  clientId;                              // NULL: coraine-aas-<8 hex digits>, random
} AasPushConfig;



// -----------------------------------------------------------------------------
//
// aasPushStart - url: mqtt://<host>[:<port>] (1883), mqtts://<host>[:<port>] (8883). NULL: started (connected or
// not - it is retried); else why not (a malformed url, a file TLS cannot use)
//
extern const char* aasPushStart(const AasPushConfig* cP);



// -----------------------------------------------------------------------------
//
// aasPushStop - disconnected, the thread joined: no event after it returns
//
extern void aasPushStop(void);

#endif  // AAS_PUSH_H_
