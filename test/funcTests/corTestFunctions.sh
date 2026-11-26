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
# mockStart [--https] [--authorization <value>] [--values-as-text] - the mock AAS server (aasMock.py) on port $AAS_MOCK_PORT
#
# --https: with a certificate of its own, made here - issued to localhost and signed by a root of the test's
# ($AAS_TEST_DIR/ca.pem). Its PATCHes and invocations go to $AAS_TEST_DIR/server.log.
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
      --values-as-text)
        opts+=(--values-as-text)
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
