# SPDX-License-Identifier: Apache-2.0
#
# corTestFunctions.sh - the verbs this repo's tests use; corTest sources it into every section
#
# A test starts a broker that loads THIS build of the plugin (by path - nothing is installed), on a port
# of its own, with ramDB as its store: no database server is needed. A test waits on and asserts on what
# the API says (and on what the mock AAS server was asked), never on the broker's log - the log is kept
# for reading when a test fails.
#
#   AAS_BROKER     the broker binary (default: coraine on PATH - the coraine package, or
#                  `. <dir>/install/env` after coraine-build)
#   AAS_PORT       its port (default 19030 - clear of a broker on 1026, of coraine's own test ports and of
#                  the other bridges' tests)
#   AAS_PLUGIN     the plugin under test (default: aas.so in this repo)
#   AAS_TEST_DIR   scratch directory: the bridge configuration, the mock server's log, the broker's pid and log
#
AAS_BROKER="${AAS_BROKER:-coraine}"
AAS_PORT="${AAS_PORT:-19030}"
AAS_PLUGIN="${AAS_PLUGIN:-${REPO_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}/aas.so}"
AAS_TEST_DIR="${AAS_TEST_DIR:-/tmp/aas-bridge-test}"


# -----------------------------------------------------------------------------
#
# brokerStart [broker options...] - start the broker with the plugin and $AAS_TEST_DIR/bridges.json
#
# Returns once the port answers; 1 (with the log's tail on stderr) if the broker died on the way.
#
brokerStart() {
  brokerStop

  "$AAS_BROKER" --port "$AAS_PORT" --pretty-print 2 --httpEndpoint "http://localhost:$AAS_PORT" \
                --database ramDB --bridges "$AAS_PLUGIN" --bridgeConfig "$AAS_TEST_DIR/bridges.json" "$@" \
                > "$AAS_TEST_DIR/broker.log" 2>&1 &
  echo $! > "$AAS_TEST_DIR/broker.pid"

  local i
  for ((i = 0; i < 100; i++)); do
    curl -s -o /dev/null "localhost:$AAS_PORT/ngsi-ld/v1/types" && return 0
    if ! kill -0 "$(cat "$AAS_TEST_DIR/broker.pid")" 2>/dev/null; then
      echo "brokerStart: the broker died - $AAS_TEST_DIR/broker.log:" >&2
      tail -20 "$AAS_TEST_DIR/broker.log" >&2
      return 1
    fi
    sleep 0.1
  done

  echo "brokerStart: no answer on port $AAS_PORT after 10s" >&2
  return 1
}


# -----------------------------------------------------------------------------
#
# brokerStop - stop the broker brokerStart started (by its pid, never by name)
#
brokerStop() {
  local pidFile="$AAS_TEST_DIR/broker.pid"
  [ -f "$pidFile" ] || return 0

  local pid i
  pid=$(cat "$pidFile")
  kill "$pid" 2>/dev/null
  for ((i = 0; i < 50; i++)); do
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.1
  done
  kill -9 "$pid" 2>/dev/null
  rm -f "$pidFile"
}


# -----------------------------------------------------------------------------
#
# mockStart [--https [--client-ca]] [--authorization <value>] [--oauth2 <id>:<secret> [--token-life <s>]] [--values-as-text]
# - the mock AAS server (aasMock.py) on port $AAS_MOCK_PORT
#
# --https: with a certificate of its own, made here - issued to localhost and signed by a root of the test's
# ($AAS_TEST_DIR/ca.pem). --client-ca: it takes only a client with a certificate of that root - one is made,
# $AAS_TEST_DIR/client.pem and client.key. --oauth2: also a token endpoint, POST /token (aasMock.py). Its
# PATCHes, invocations and tokens go to $AAS_TEST_DIR/server.log.
#
AAS_MOCK_PORT="${AAS_MOCK_PORT:-18081}"

mockStart() {
  mockStop

  local opts=()
  while [ $# -gt 0 ]; do
    case "$1" in
      --https)
        local d="$AAS_TEST_DIR"
        openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj "/CN=aas test root" \
                -keyout "$d/ca.key" -out "$d/ca.pem" > /dev/null 2>&1
        openssl req -newkey rsa:2048 -nodes -subj "/CN=localhost" \
                -keyout "$d/server.key" -out "$d/server.csr" > /dev/null 2>&1
        printf 'subjectAltName=DNS:localhost\n' > "$d/server.ext"
        openssl x509 -req -in "$d/server.csr" -CA "$d/ca.pem" -CAkey "$d/ca.key" -CAcreateserial -days 2 \
                -extfile "$d/server.ext" -out "$d/server.pem" > /dev/null 2>&1
        opts+=(--cert "$d/server.pem" --keyfile "$d/server.key")
        ;;
      --authorization)
        opts+=(--authorization "$2")
        shift
        ;;
      --client-ca)                                    # after --https: the test CA issues the client a certificate too
        local d="$AAS_TEST_DIR"
        openssl req -newkey rsa:2048 -nodes -subj "/CN=aas test client" \
                -keyout "$d/client.key" -out "$d/client.csr" > /dev/null 2>&1
        openssl x509 -req -in "$d/client.csr" -CA "$d/ca.pem" -CAkey "$d/ca.key" -CAcreateserial -days 2 \
                -out "$d/client.pem" > /dev/null 2>&1
        opts+=(--client-ca "$d/ca.pem")
        ;;
      --oauth2|--token-life)
        opts+=("$1" "$2")
        shift
        ;;
      --push)                                         # basyx|faaast: changes published to mqttStart's broker
        opts+=(--push "$2" --mqtt-port "${AAS_MQTT_PUBLISH_PORT:-$AAS_MQTT_PORT}")
        shift
        ;;
      --values-as-text|--log-requests)
        opts+=("$1")
        ;;
    esac
    shift
  done

  python3 "$(dirname "${BASH_SOURCE[0]}")/aasMock.py" --port "$AAS_MOCK_PORT" --log "$AAS_TEST_DIR/server.log" \
          "${opts[@]}" > "$AAS_TEST_DIR/mock.log" 2>&1 &
  echo $! > "$AAS_TEST_DIR/mock.pid"

  local i
  for ((i = 0; i < 50; i++)); do
    (echo > "/dev/tcp/127.0.0.1/$AAS_MOCK_PORT") 2>/dev/null && return 0
    sleep 0.1
  done

  echo "mockStart: no answer on port $AAS_MOCK_PORT - $AAS_TEST_DIR/mock.log:" >&2
  tail -20 "$AAS_TEST_DIR/mock.log" >&2
  return 1
}


# -----------------------------------------------------------------------------
#
# mqttStart [--tls] [--auth <user>:<password>] [--client-ca] - an MQTT broker on localhost:$AAS_MQTT_PORT
# (mosquitto, verbose: its log has each SUBSCRIBE), for push. --tls: TLS only, a certificate for localhost by a
# CA of the test's own ($AAS_TEST_DIR/mqtt-ca.pem); --auth: that user only (no anonymous client); --client-ca:
# only a client with a certificate of the same CA ($AAS_TEST_DIR/mqtt-client.pem, mqtt-client.key). The client
# publishing for the mock (no TLS, no user) gets a plain listener of its own, $AAS_MQTT_PORT + 1, when the main
# one is not plain - aasMock's --mqtt-port. mqttStop. mqttSubscribed <topic> [secs] - wait until a client has
# subscribed to <topic> at it. Its listeners on localhost: IPv4 and IPv6 - "localhost" may be ::1 first (CI's
# containers), and libmosquitto tries a name's first address only
#
AAS_MQTT_PORT="${AAS_MQTT_PORT:-18883}"
mqttStart() {
  mqttStop

  local d="$AAS_TEST_DIR" tls=no auth="" clientCa=no
  while [ $# -gt 0 ]; do
    case "$1" in
      --tls)       tls=yes ;;
      --auth)      auth="$2"; shift ;;
      --client-ca) clientCa=yes ;;
    esac
    shift
  done

  local conf="$d/mqtt.conf"
  if [ "$tls" = no ] && [ -z "$auth" ]; then
    printf 'listener %s localhost\nallow_anonymous true\n' "$AAS_MQTT_PORT" > "$conf"
  else
    printf 'per_listener_settings true\nlistener %s localhost\n' "$AAS_MQTT_PORT" > "$conf"
    if [ -n "$auth" ]; then
      : > "$d/mqtt.passwd"
      mosquitto_passwd -b "$d/mqtt.passwd" "${auth%%:*}" "${auth#*:}" > /dev/null 2>&1
      printf 'allow_anonymous false\npassword_file %s\n' "$d/mqtt.passwd" >> "$conf"
    else
      printf 'allow_anonymous true\n' >> "$conf"
    fi
    if [ "$tls" = yes ]; then
      openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj "/CN=aas test mqtt root" \
              -keyout "$d/mqtt-ca.key" -out "$d/mqtt-ca.pem" > /dev/null 2>&1
      openssl req -newkey rsa:2048 -nodes -subj "/CN=localhost" -keyout "$d/mqtt-server.key" -out "$d/mqtt-server.csr" > /dev/null 2>&1
      printf 'subjectAltName=DNS:localhost\n' > "$d/mqtt-server.ext"
      openssl x509 -req -in "$d/mqtt-server.csr" -CA "$d/mqtt-ca.pem" -CAkey "$d/mqtt-ca.key" -CAcreateserial -days 2 \
              -extfile "$d/mqtt-server.ext" -out "$d/mqtt-server.pem" > /dev/null 2>&1
      printf 'cafile %s\ncertfile %s\nkeyfile %s\n' "$d/mqtt-ca.pem" "$d/mqtt-server.pem" "$d/mqtt-server.key" >> "$conf"
      if [ "$clientCa" = yes ]; then
        openssl req -newkey rsa:2048 -nodes -subj "/CN=aas test mqtt client" -keyout "$d/mqtt-client.key" -out "$d/mqtt-client.csr" > /dev/null 2>&1
        openssl x509 -req -in "$d/mqtt-client.csr" -CA "$d/mqtt-ca.pem" -CAkey "$d/mqtt-ca.key" -CAcreateserial -days 2 \
                -out "$d/mqtt-client.pem" > /dev/null 2>&1
        printf 'require_certificate true\n' >> "$conf"
      fi
      chmod 644 "$d"/mqtt-*.key
    fi
    printf 'listener %s localhost\nallow_anonymous true\n' "$((AAS_MQTT_PORT + 1))" >> "$conf"
  fi

  mosquitto -v -c "$conf" > "$AAS_TEST_DIR/mqtt.log" 2>&1 &
  echo $! > "$AAS_TEST_DIR/mqtt.pid"

  local i
  for ((i = 0; i < 50; i++)); do
    (echo > "/dev/tcp/127.0.0.1/$AAS_MQTT_PORT") 2>/dev/null && return 0
    sleep 0.1
  done

  echo "mqttStart: no answer on port $AAS_MQTT_PORT - $AAS_TEST_DIR/mqtt.log:" >&2
  tail -20 "$AAS_TEST_DIR/mqtt.log" >&2
  return 1
}

mqttSubscribed() {
  local secs="${2:-10}" i

  for ((i = 0; i < secs * 10; i++)); do
    grep -qF -- "	$1 (QoS" "$AAS_TEST_DIR/mqtt.log" && return 0
    sleep 0.1
  done

  echo "mqttSubscribed: no client subscribed to '$1' after ${secs}s - $AAS_TEST_DIR/mqtt.log" >&2
  return 1
}

mqttStop() {
  local pidFile="$AAS_TEST_DIR/mqtt.pid" pid i
  [ -f "$pidFile" ] || return 0

  pid=$(cat "$pidFile")
  kill "$pid" 2>/dev/null
  for ((i = 0; i < 50; i++)); do
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.1
  done
  rm -f "$pidFile"
}


# -----------------------------------------------------------------------------
#
# mockStop - stop the mock mockStart started (by its pid)
#
mockStop() {
  local pidFile="$AAS_TEST_DIR/mock.pid" pid i
  [ -f "$pidFile" ] || return 0

  pid=$(cat "$pidFile")
  kill "$pid" 2>/dev/null
  for ((i = 0; i < 50; i++)); do
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.1
  done
  rm -f "$pidFile"
}


# -----------------------------------------------------------------------------
#
# mockSet <submodel id> <idShortPath> <value as JSON> - an element's value changed at the source
#
mockSet() {
  curl -s -o /dev/null "localhost:$AAS_MOCK_PORT/test/set" -d "{\"submodel\": \"$1\", \"path\": \"$2\", \"value\": $3}"
}


# -----------------------------------------------------------------------------
#
# mockAdd <submodel id> <element as JSON> / mockRemove <submodel id> <idShort> - the model's structure changed
#
mockAdd() {
  curl -s -o /dev/null "localhost:$AAS_MOCK_PORT/test/add" -d "{\"submodel\": \"$1\", \"element\": $2}"
}

mockRemove() {
  curl -s -o /dev/null "localhost:$AAS_MOCK_PORT/test/remove" -d "{\"submodel\": \"$1\", \"idShort\": \"$2\"}"
}


# -----------------------------------------------------------------------------
#
# serverAwait <line> [secs] - wait for the mock server to have been asked <line> (a PATCH, an invocation)
#
serverAwait() {
  local secs="${2:-10}" i

  for ((i = 0; i < secs * 10; i++)); do
    grep -qxF -- "$1" "$AAS_TEST_DIR/server.log" 2>/dev/null && { echo "$1"; return 0; }
    sleep 0.1
  done

  echo "serverAwait: no '$1' after ${secs}s - the server was asked: $(cat "$AAS_TEST_DIR/server.log" 2>/dev/null)" >&2
  return 1
}


# -----------------------------------------------------------------------------
#
# serverLog - what the mock server was asked
#
serverLog() {
  cat "$AAS_TEST_DIR/server.log" 2>/dev/null
}


# -----------------------------------------------------------------------------
#
# entityAwait <id> <attr> <text> [secs] - wait until the entity's attr (keyValues) has <text>
#
entityAwait() {
  local url="localhost:$AAS_PORT/ngsi-ld/v1/entities/$(urlEncode "$1")?options=keyValues&attrs=$2" secs="${4:-10}" i

  for ((i = 0; i < secs * 10; i++)); do
    curl -s "$url" | grep -qF -- "$3" && return 0
    sleep 0.1
  done

  echo "entityAwait: $1/$2 is $(curl -s "$url"), without '$3', after ${secs}s" >&2
  return 1
}


# -----------------------------------------------------------------------------
#
# entityGet <id> [attrs] [options] - the entity, its members sorted
#
entityGet() {
  local q=""

  [ -n "$2" ] && q="attrs=$2"
  [ -n "$3" ] && q="${q:+$q&}options=$3"
  curl -s "localhost:$AAS_PORT/ngsi-ld/v1/entities/$(urlEncode "$1")${q:+?$q}" \
    | python3 -c 'import sys, json; print(json.dumps(json.load(sys.stdin), indent=2, sort_keys=True))'
}


# -----------------------------------------------------------------------------
#
# attrPatch <id> <attr> <body> - PATCH an attribute; the HTTP status
#
attrPatch() {
  curl -s -o /dev/null -w "%{http_code}\n" -X PATCH "localhost:$AAS_PORT/ngsi-ld/v1/entities/$(urlEncode "$1")/attrs/$2" \
       -H 'Content-Type: application/json' -d "$3"
}


# -----------------------------------------------------------------------------
#
# attrPatchShow <id> <attr> <body> - as attrPatch, then the error's detail (if any)
#
attrPatchShow() {
  curl -s -w "\n%{http_code}\n" -X PATCH "localhost:$AAS_PORT/ngsi-ld/v1/entities/$(urlEncode "$1")/attrs/$2" \
       -H 'Content-Type: application/json' -d "$3" > "$AAS_TEST_DIR/patch.out"
  tail -1 "$AAS_TEST_DIR/patch.out"
  head -n -1 "$AAS_TEST_DIR/patch.out" | python3 -c '
import json, sys
t = sys.stdin.read().strip()
if t:
    print(json.dumps(json.loads(t).get("detail")))'
}


# -----------------------------------------------------------------------------
#
# assetChannels - each AssetChannel: its target, status (and why), direction, counters
#
assetChannels() {
  curl -s "localhost:$AAS_PORT/ngsi-ld/v1/assetChannels" | python3 -c '
import json, sys
for c in sorted(json.load(sys.stdin), key=lambda c: c["channelTarget"]):
    print(c["channelTarget"], c.get("status"), c.get("statusReason", "-"), c.get("channelDirection", "in"),
          "valuesIn=%s valuesOut=%s" % (c.get("valuesIn"), c.get("valuesOut", "-")))'
}


# -----------------------------------------------------------------------------
#
# channelAwait <target> <text> [secs] - wait until the AssetChannel's line (assetChannels) has <text>
#
channelAwait() {
  local secs="${3:-10}" i

  for ((i = 0; i < secs * 10; i++)); do
    assetChannels | grep -F -- "$1 " | grep -qF -- "$2" && return 0
    sleep 0.1
  done

  echo "channelAwait: $1 never had '$2' - $(assetChannels)" >&2
  return 1
}


# -----------------------------------------------------------------------------
#
# urlEncode <text> - percent-encoded, for an entity id in a path
#
urlEncode() {
  python3 -c 'import sys, urllib.parse; print(urllib.parse.quote(sys.argv[1], safe=""))' "$1"
}


# -----------------------------------------------------------------------------
#
# basyxEnv - AAS_BASYX_URL, where the Eclipse BaSyx AAS environment is ($AAS_BASYX, corTest's -basyx: a URL as
# it is, an image on 127.0.0.1:$AAS_BASYX_PORT), and AAS_DELEGATE_HOST, the address BaSyx reaches the test's
# host on: docker's bridge gateway, or (a URL: CI's job container) this host's first address. Each helper below
# calls it: a test's sections are processes of their own
#
# basyxStart - basyxEnv; an image started in docker, its Operation delegation allowed to private addresses (the
# test's delegate); returns once BaSyx answers
#
AAS_BASYX_PORT="${AAS_BASYX_PORT:-18082}"
AAS_DELEGATE_PORT="${AAS_DELEGATE_PORT:-18083}"

basyxEnv() {
  case "$AAS_BASYX" in
    http://*|https://*)
      AAS_BASYX_URL="$AAS_BASYX"
      AAS_DELEGATE_HOST="${AAS_DELEGATE_HOST:-$(hostname -I | awk '{print $1}')}"
      ;;
    *)
      AAS_BASYX_URL="http://127.0.0.1:$AAS_BASYX_PORT"
      AAS_DELEGATE_HOST="${AAS_DELEGATE_HOST:-$(docker network inspect bridge -f '{{(index .IPAM.Config 0).Gateway}}' 2>/dev/null)}"
      ;;
  esac
  export AAS_BASYX_URL AAS_DELEGATE_HOST
}

basyxStart() {
  basyxEnv
  case "$AAS_BASYX" in
    http://*|https://*) ;;
    *)
      docker rm -f aas-test-basyx > /dev/null 2>&1
      docker run -d --rm --name aas-test-basyx -p "127.0.0.1:$AAS_BASYX_PORT:8081" \
             -e BASYX_SUBMODELREPOSITORY_FEATURE_OPERATION_DELEGATION_SECURITY_DENYPRIVATETARGETS=false \
             "$AAS_BASYX" > "$AAS_TEST_DIR/basyx.id" 2>&1 || { cat "$AAS_TEST_DIR/basyx.id" >&2; return 1; }
      ;;
  esac

  local i
  for ((i = 0; i < 120; i++)); do
    curl -sf -o /dev/null "$AAS_BASYX_URL/shells" && return 0
    sleep 0.5
  done

  echo "basyxStart: no answer from $AAS_BASYX_URL after 60s" >&2
  return 1
}


# -----------------------------------------------------------------------------
#
# basyxStop - the container basyxStart started (a URL's server is left running)
#
basyxStop() {
  case "$AAS_BASYX" in
    http://*|https://*) ;;
    *) docker rm -f aas-test-basyx > /dev/null 2>&1 ;;
  esac
  return 0
}


# -----------------------------------------------------------------------------
#
# basyxDelete <shells|submodels> <id> / basyxPost <shells|submodels|submodels/<id>/submodel-elements> <json> -
# the BaSyx API; the HTTP status
#
basyxDelete() {
  basyxEnv
  curl -s -o /dev/null -w "%{http_code}\n" -X DELETE "$AAS_BASYX_URL/$1/$(b64url "$2")"
}

basyxPost() {
  basyxEnv
  curl -s -o "$AAS_TEST_DIR/basyx.out" -w "%{http_code}\n" -X POST "$AAS_BASYX_URL/$1" -H 'Content-Type: application/json' -d "$2"
}


# -----------------------------------------------------------------------------
#
# faaastEnv / faaastStart / faaastStop / faaastPost - FA³ST Service, as basyx*: AAS_FAAAST a URL (left running) or
# an image, started on 127.0.0.1:$AAS_FAAAST_PORT with its defaults - https (a certificate of its own, self-signed),
# the API under /api/v3.0, an empty model. AAS_FAAAST_URL: where its API is
#
AAS_FAAAST_PORT="${AAS_FAAAST_PORT:-18084}"

faaastEnv() {
  case "$AAS_FAAAST" in
    http://*|https://*) AAS_FAAAST_URL="$AAS_FAAAST" ;;
    *)                  AAS_FAAAST_URL="https://localhost:$AAS_FAAAST_PORT/api/v3.0" ;;
  esac
  export AAS_FAAAST_URL
}

faaastStart() {
  faaastEnv
  case "$AAS_FAAAST" in
    http://*|https://*) ;;
    *)
      docker rm -f aas-test-faaast > /dev/null 2>&1
      docker run -d --rm --name aas-test-faaast -p "127.0.0.1:$AAS_FAAAST_PORT:443" "$AAS_FAAAST" \
             > "$AAS_TEST_DIR/faaast.id" 2>&1 || { cat "$AAS_TEST_DIR/faaast.id" >&2; return 1; }
      ;;
  esac

  local i
  for ((i = 0; i < 120; i++)); do
    curl -skf -o /dev/null "$AAS_FAAAST_URL/shells" && return 0
    sleep 0.5
  done

  echo "faaastStart: no answer from $AAS_FAAAST_URL after 60s" >&2
  return 1
}

faaastStop() {
  case "$AAS_FAAAST" in
    http://*|https://*) ;;
    *) docker rm -f aas-test-faaast > /dev/null 2>&1 ;;
  esac
  return 0
}

faaastDelete() {
  faaastEnv
  curl -sk -o /dev/null -w "%{http_code}\n" -X DELETE "$AAS_FAAAST_URL/$1/$(b64url "$2")"
}

faaastPost() {
  faaastEnv
  curl -sk -o "$AAS_TEST_DIR/faaast.out" -w "%{http_code}\n" -X POST "$AAS_FAAAST_URL/$1" -H 'Content-Type: application/json' -d "$2"
}


# -----------------------------------------------------------------------------
#
# b64url <id> - base64url, no padding: an AAS id in a path
#
b64url() {
  python3 -c 'import base64, sys; print(base64.urlsafe_b64encode(sys.argv[1].encode()).decode().rstrip("="))' "$1"
}


# -----------------------------------------------------------------------------
#
# delegateStart / delegateStop - basyxDelegate.py on $AAS_DELEGATE_HOST:$AAS_DELEGATE_PORT: what BaSyx invokes
# for an Operation it delegates (the Operation's qualifier invocationDelegation)
#
delegateStart() {
  basyxEnv
  delegateStop
  python3 "$(dirname "${BASH_SOURCE[0]}")/basyxDelegate.py" --host "$AAS_DELEGATE_HOST" --port "$AAS_DELEGATE_PORT" \
          --log "$AAS_TEST_DIR/delegate.log" > "$AAS_TEST_DIR/delegate.out" 2>&1 &
  echo $! > "$AAS_TEST_DIR/delegate.pid"

  local i
  for ((i = 0; i < 50; i++)); do
    (echo > "/dev/tcp/$AAS_DELEGATE_HOST/$AAS_DELEGATE_PORT") 2>/dev/null && return 0
    sleep 0.1
  done

  echo "delegateStart: no answer on $AAS_DELEGATE_HOST:$AAS_DELEGATE_PORT - $AAS_TEST_DIR/delegate.out:" >&2
  tail -20 "$AAS_TEST_DIR/delegate.out" >&2
  return 1
}

delegateStop() {
  local pidFile="$AAS_TEST_DIR/delegate.pid"
  [ -f "$pidFile" ] || return 0
  kill "$(cat "$pidFile")" 2>/dev/null
  rm -f "$pidFile"
}
