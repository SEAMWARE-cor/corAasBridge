#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# aasMock.py - an AAS server's HTTP/REST API (IDTA Part 2, V3), as much of it as the aas plugin uses
#
#   GET   /shells, /submodels                                   { "result": [ ... ], "paging_metadata": {} }
#   GET   /shells/<id>, /submodels/<id>                         one; the id base64url-encoded (padded or not)
#   GET   /submodels/<id>/submodel-elements/<idShortPath>       one element
#   PATCH /submodels/<id>/submodel-elements/<idShortPath>/$value  the ValueOnly value: 204, or 400 when it does
#                                                               not fit the element (as a server checks it)
#   POST  /submodels/<id>/submodel-elements/<idShortPath>/invoke  an Operation, synchronously: an OperationResult
#
# The press's Operations: SetSpeed (speed xs:double, ramp xs:boolean -> previous xs:double), Stop (fails: the
# press is not running), Log (lines xs:string -> count xs:int). A Mode written "broken" is refused (400) - a
# value the plugin cannot know is wrong.
#
# With --values-as-text, a PATCH takes numbers and booleans only as strings ("421", "true") and answers 500 to
# a JSON number or boolean - as Eclipse BaSyx does (eclipse-basyx/basyx-java-server-sdk#384, #403), where
# IDTA-01001's ValueOnly has them as JSON numbers and booleans.
#
# With --authorization, a request without that Authorization header is answered 401. With --oauth2 <id>:<secret>
# the server is also an OAuth2 token endpoint, POST /token (the client credentials grant, the client's id and
# secret as HTTP Basic): it gives "tok-<n>", valid --token-life seconds (60 by default); a request without a
# valid one, "Bearer tok-<n>", is answered 401. Each token given is logged, "token <n> scope=<scope>", and each
# request refused, "refused <its Authorization>". With --client-ca <pem> (and --cert), a client without a
# certificate that CA issued is refused at the handshake.
# The test's own verbs, not the API's:
#
#   POST /test/set      { "submodel", "path", "value" }   an element's value changed at the source (the AAS JSON
#                                                          form: a Property's a string, an MLP's [ { language, text } ])
#   POST /test/add      { "submodel", "element" }         an element added - the model's structure changes
#   POST /test/remove   { "submodel", "idShort" }         one removed
#   POST /test/revoke   {}                                every token given so far no longer valid
#
# Each PATCH, PUT and invocation is logged, one line, to the file --log names:
# "PATCH <submodel idShort>.<idShortPath> <body>", "PUT <submodel idShort>.<idShortPath> <the items' values>" (a
# list, PutSubmodelElementByPath), "invoke <submodel idShort>.<idShortPath> <arguments>".
#
#   aasMock.py --port <p> --log <file> [--values-as-text] [--authorization <value>] [--cert <pem> --keyfile <pem>]
#              [--client-ca <pem>] [--oauth2 <id>:<secret> [--token-life <s>]]
#
import argparse
import base64
import json
import ssl
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

NAMEPLATE = "https://acme.example/ids/sm/press-17/nameplate"
OPERATIONAL = "https://acme.example/ids/sm/press-17/operational"
SHELL = "https://acme.example/ids/aas/press-17"
LINE3 = "https://acme.example/ids/aas/line-3"
LAYOUT = "https://acme.example/ids/sm/line-3/layout"


def ext(value):
  return {"type": "ExternalReference", "keys": [{"type": "GlobalReference", "value": value}]}


def prop(idShort, valueType, value=None, semanticId=None):
  e = {"modelType": "Property", "idShort": idShort, "valueType": valueType}
  if value is not None:
    e["value"] = value
  if semanticId is not None:
    e["semanticId"] = ext(semanticId)
  return e


def mlp(idShort, texts, semanticId=None):
  e = {"modelType": "MultiLanguageProperty", "idShort": idShort,
       "value": [{"language": l, "text": t} for l, t in texts]}
  if semanticId is not None:
    e["semanticId"] = ext(semanticId)
  return e


def mref(*keys):
  # a ModelReference - its keys (type, value)
  return {"type": "ModelReference", "keys": [{"type": t, "value": v} for t, v in keys]}


def rel(idShort, first, second):
  e = {"modelType": "RelationshipElement", "idShort": idShort, "first": first}
  if second is not None:
    e["second"] = second
  return e


def var(idShort, valueType):
  return {"value": {"modelType": "Property", "idShort": idShort, "valueType": valueType}}


SHELLS = [
  {"modelType": "AssetAdministrationShell", "id": SHELL, "idShort": "Press17",
   "assetInformation": {"assetKind": "Instance", "globalAssetId": "https://acme.example/assets/press-17"},
   "submodels": [{"type": "ModelReference", "keys": [{"type": "Submodel", "value": NAMEPLATE}]},
                 {"type": "ModelReference", "keys": [{"type": "Submodel", "value": OPERATIONAL}]}]},
  {"modelType": "AssetAdministrationShell", "id": "https://acme.example/ids/aas/press-18", "idShort": "Press18",
   "assetInformation": {"assetKind": "Instance"},
   "submodels": [{"type": "ModelReference", "keys": [{"type": "Submodel", "value": "https://acme.example/ids/sm/none"}]}]},
  {"modelType": "AssetAdministrationShell", "id": LINE3, "idShort": "Line3",
   "assetInformation": {"assetKind": "Instance"},
   "submodels": [mref(("Submodel", LAYOUT))]}
]

SUBMODELS = [
  {"modelType": "Submodel", "id": NAMEPLATE, "idShort": "Nameplate",
   "semanticId": ext("https://admin-shell.io/idta/nameplate/3/0/Nameplate"),
   "submodelElements": [
     mlp("ManufacturerName", [("en", "ACME Presses"), ("de", "ACME Pressen")], "0112/2///61987#ABA565#009"),
     prop("SerialNumber", "xs:string", "P17-0042", "0112/2///61987#ABA951#009"),
     prop("YearOfConstruction", "xs:int", "2024"),
     prop("NoValue", "xs:string"),
     {"modelType": "SubmodelElementCollection", "idShort": "AddressInformation", "value": [
       mlp("CityTown", [("de", "Berlin")]),
       prop("Street", "xs:string", "Hauptstr. 1")]},
     {"modelType": "SubmodelElementList", "idShort": "Markings", "typeValueListElement": "Property",
      "valueTypeListElement": "xs:string", "value": [
        {"modelType": "Property", "valueType": "xs:string", "value": "CE"},
        {"modelType": "Property", "valueType": "xs:string", "value": "UKCA"}]},
     {"modelType": "SubmodelElementList", "idShort": "Languages", "typeValueListElement": "SubmodelElement", "value": [
        {"modelType": "Property", "valueType": "xs:string", "value": "en"},
        {"modelType": "Property", "valueType": "xs:string", "value": "de"}]},
     {"modelType": "File", "idShort": "Manual", "contentType": "application/pdf", "value": "/aasx/manual.pdf"},
     {"modelType": "File", "idShort": "Logo", "contentType": "image/png", "value": "https://acme.example/logo.png"},
     {"modelType": "Blob", "idShort": "Thumbnail", "contentType": "image/png", "value": "iVBORw0KGgo="},
     {"modelType": "Capability", "idShort": "Pressing"}]},
  {"modelType": "Submodel", "id": OPERATIONAL, "idShort": "OperationalData",
   "submodelElements": [
     prop("Pressure", "xs:double", "212.5"),
     prop("Running", "xs:boolean", "true"),
     prop("Mode", "xs:string", "auto"),
     {"modelType": "Range", "idShort": "PressureRange", "valueType": "xs:double", "min": "0", "max": "400"},
     {"modelType": "ReferenceElement", "idShort": "Documentation",
      "value": {"type": "ModelReference", "keys": [{"type": "Submodel", "value": NAMEPLATE}]}},
     {"modelType": "ReferenceElement", "idShort": "Certificate", "value": ext("https://certs.example/p17")},
     {"modelType": "ReferenceElement", "idShort": "Street",
      "value": {"type": "ModelReference", "keys": [{"type": "Submodel", "value": NAMEPLATE},
                                                   {"type": "SubmodelElementCollection", "value": "AddressInformation"},
                                                   {"type": "Property", "value": "Street"}]}},
     {"modelType": "SubmodelElementList", "idShort": "Stations", "typeValueListElement": "SubmodelElementCollection", "value": [
       {"modelType": "SubmodelElementCollection", "value": [prop("Name", "xs:string", "Infeed")]},
       {"modelType": "SubmodelElementCollection", "value": [prop("Name", "xs:string", "Outfeed")]}]},
     {"modelType": "ReferenceElement", "idShort": "LastStation",
      "value": {"type": "ModelReference", "keys": [{"type": "Submodel", "value": OPERATIONAL},
                                                   {"type": "SubmodelElementList", "value": "Stations"},
                                                   {"type": "SubmodelElementCollection", "value": "1"}]}},
     {"modelType": "Operation", "idShort": "SetSpeed",
      "inputVariables": [var("speed", "xs:double"), var("ramp", "xs:boolean")],
      "outputVariables": [var("previous", "xs:double")]},
     {"modelType": "Operation", "idShort": "Stop"},
     {"modelType": "Operation", "idShort": "Log", "inputVariables": [var("lines", "xs:string")],
      "outputVariables": [var("count", "xs:int")]}]},
  {"modelType": "Submodel", "id": LAYOUT, "idShort": "Layout",
   "submodelElements": [
     prop("Speed", "xs:double", "1.2"),
     {"modelType": "SubmodelElementCollection", "idShort": "Infeed", "value": [prop("Rate", "xs:int", "40")]},
     rel("Feeds",     mref(("Submodel", LAYOUT)),                                           mref(("AssetAdministrationShell", SHELL))),
     rel("Drives",    mref(("Submodel", LAYOUT), ("Property", "Speed")),                    mref(("Submodel", OPERATIONAL), ("Property", "Pressure"))),
     rel("LimitedBy", mref(("Submodel", LAYOUT), ("SubmodelElementCollection", "Infeed"),
                           ("Property", "Rate")),                                           mref(("Submodel", LAYOUT), ("Property", "Speed"))),
     rel("LocatedIn", mref(("AssetAdministrationShell", LINE3)),                            ext("https://sites.example/hall-2")),
     rel("Supplies",  mref(("Submodel", NAMEPLATE)),                                        mref(("AssetAdministrationShell", LINE3))),
     rel("Dangling",  mref(("Submodel", LAYOUT)),                                           None)]}
]

lock  = threading.Lock()
tokens = {}                                    # "tok-<n>": when it expires
speed = [0.0]
args  = None


def b64decode(s):
  s = urllib.parse.unquote(s)
  return base64.urlsafe_b64decode(s + "=" * (-len(s) % 4)).decode()


def byId(items, id):
  for i in items:
    if i["id"] == id:
      return i
  return None


def element(sm, path):
  # "AddressInformation.Street", "Markings[1]"
  path = urllib.parse.unquote(path)
  e = {"value": sm["submodelElements"]}
  for part in path.split("."):
    idShort, _, rest = part.partition("[")
    items = e.get("value") if isinstance(e.get("value"), list) else []
    e = next((x for x in items if x.get("idShort") == idShort), None)
    if e is None:
      return None
    while rest:
      ix, _, rest = rest.partition("]")
      rest = rest.lstrip("[")
      items = e.get("value") if isinstance(e.get("value"), list) else []
      e = items[int(ix)] if int(ix) < len(items) else None
      if e is None:
        return None
  return e


def fits(valueType, v):
  if valueType in ("xs:int", "xs:integer", "xs:long", "xs:short", "xs:byte"):
    return isinstance(v, int) and not isinstance(v, bool)
  if valueType in ("xs:double", "xs:float", "xs:decimal"):
    return isinstance(v, (int, float)) and not isinstance(v, bool)
  if valueType == "xs:boolean":
    return isinstance(v, bool)
  return isinstance(v, str)


def text(v):
  return ("true" if v else "false") if isinstance(v, bool) else str(v)


def valueSet(e, v):
  # a ValueOnly value into the element; False: it does not fit
  t = e["modelType"]
  if t == "Property":
    if not fits(e["valueType"], v) or v == "broken":
      return False
    e["value"] = text(v)
  elif t == "MultiLanguageProperty":
    if not isinstance(v, list) or not all(isinstance(x, dict) and len(x) == 1 for x in v):
      return False
    e["value"] = [{"language": l, "text": t} for x in v for l, t in x.items()]
  elif t == "Range":
    if not isinstance(v, dict) or not all(k in ("min", "max") and fits(e["valueType"], x) for k, x in v.items()):
      return False
    for k, x in v.items():
      e[k] = text(x)
  elif t == "SubmodelElementList" and listValueType(e) is not None:
    vt = listValueType(e)
    if not isinstance(v, list) or not all(fits(vt, x) for x in v):
      return False
    e["value"] = [{"modelType": "Property", "valueType": vt, "value": text(x)} for x in v]
  else:
    return False
  return True


def listValueType(e):
  # a list of Properties - declared so, or of any element with Property items: their valueType
  if e.get("typeValueListElement") == "Property":
    return e["valueTypeListElement"]
  items = e.get("value") or []
  if e.get("typeValueListElement") == "SubmodelElement" and items and all(x["modelType"] == "Property" for x in items):
    return items[0]["valueType"]
  return None


def scalars(v):
  # the scalars of a ValueOnly value
  if isinstance(v, list):
    return [x for i in v for x in scalars(i)]
  if isinstance(v, dict):
    return [x for i in v.values() for x in scalars(i)]
  return [v]


def fromText(e, v):
  # a ValueOnly value whose scalars are all strings, typed by the element's valueType (--values-as-text);
  # None: one is not a string, or does not read as its type
  def typed(valueType, x):
    if not isinstance(x, str):
      raise ValueError
    if valueType in ("xs:int", "xs:integer", "xs:long", "xs:short", "xs:byte"):
      return int(x)
    if valueType in ("xs:double", "xs:float", "xs:decimal"):
      return float(x)
    if valueType == "xs:boolean":
      return {"true": True, "false": False}[x]
    return x
  t = e["modelType"]
  try:
    if t == "Property":
      return typed(e["valueType"], v)
    if t == "Range" and isinstance(v, dict):
      return {k: typed(e["valueType"], x) for k, x in v.items()}
    if t == "SubmodelElementList" and isinstance(v, list) and listValueType(e) is not None:
      return [typed(listValueType(e), x) for x in v]
  except (ValueError, KeyError):
    return None
  return v


def operation(name, inputs):
  # an OperationResult
  def out(idShort, valueType, value):
    return {"value": {"modelType": "Property", "idShort": idShort, "valueType": valueType, "value": value}}

  if name == "SetSpeed":
    previous = speed[0]
    speed[0] = float(inputs["speed"])
    return {"executionState": "Completed", "success": True, "outputArguments": [out("previous", "xs:double", repr(previous))]}
  if name == "Stop":
    return {"executionState": "Failed", "success": False, "messages": [{"messageType": "Error", "text": "the press is not running"}]}
  if name == "Log":
    return {"executionState": "Completed", "success": True, "outputArguments": [out("count", "xs:int", str(len(inputs["lines"].split(","))))]}
  return {"executionState": "Failed", "success": False}


class Handler(BaseHTTPRequestHandler):
  protocol_version = "HTTP/1.1"

  def log_message(self, fmt, *a):
    pass

  def answer(self, status, obj=None):
    body = json.dumps(obj).encode() if obj is not None else b""
    self.send_response(status)
    if obj is not None:
      self.send_header("Content-Type", "application/json")
    self.send_header("Content-Length", str(len(body)))
    self.end_headers()
    self.wfile.write(body)

  def notFound(self):
    self.answer(404, {"messages": [{"messageType": "Error", "text": "not found"}]})

  def body(self):
    n = int(self.headers.get("Content-Length", "0"))
    return self.rfile.read(n) if n > 0 else b""

  def authorized(self):
    given = self.headers.get("Authorization") or ""
    if args.oauth2 is not None:
      with lock:
        if given.startswith("Bearer ") and tokens.get(given[7:], 0) > time.time():
          return True
    elif args.authorization is None or given == args.authorization:
      return True
    if args.oauth2 is not None:
      self.logLine("refused %s" % given)
    self.answer(401, {"messages": [{"messageType": "Error", "text": "unauthorized"}]})
    return False

  def logLine(self, line):
    with open(args.log, "a") as f:
      f.write(line + "\n")

  def parts(self):
    return self.path.split("?")[0].strip("/").split("/")

  def do_GET(self):
    if not self.authorized():
      return
    p = self.parts()
    with lock:
      if p == ["shells"]:
        return self.answer(200, {"result": SHELLS, "paging_metadata": {}})
      if p == ["submodels"]:
        return self.answer(200, {"result": SUBMODELS, "paging_metadata": {}})
      try:
        if len(p) == 2 and p[0] in ("shells", "submodels"):
          i = byId(SHELLS if p[0] == "shells" else SUBMODELS, b64decode(p[1]))
          return self.answer(200, i) if i is not None else self.notFound()
        if len(p) == 4 and p[0] == "submodels" and p[2] == "submodel-elements":
          sm = byId(SUBMODELS, b64decode(p[1]))
          e = element(sm, p[3]) if sm is not None else None
          return self.answer(200, e) if e is not None else self.notFound()
      except (ValueError, UnicodeDecodeError):
        return self.answer(400, {"messages": [{"messageType": "Error", "text": "not base64url"}]})
    self.notFound()

  def do_PATCH(self):
    raw = self.body()
    if not self.authorized():
      return
    p = self.parts()
    if len(p) != 5 or p[0] != "submodels" or p[2] != "submodel-elements" or p[4] != "$value":
      return self.notFound()
    with lock:
      sm = byId(SUBMODELS, b64decode(p[1]))
      e = element(sm, p[3]) if sm is not None else None
      if e is None:
        return self.notFound()
      self.logLine("PATCH %s.%s %s" % (sm["idShort"], urllib.parse.unquote(p[3]), raw.decode()))
      try:
        v = json.loads(raw)
        if args.values_as_text and e["modelType"] != "MultiLanguageProperty":
          if any(not isinstance(x, str) for x in scalars(v)):
            return self.answer(500, {"status": 500, "error": "Internal Server Error"})
          v = fromText(e, v)
        ok = (v is not None) and valueSet(e, v)
      except ValueError:
        ok = False
    if not ok:
      return self.answer(400, {"messages": [{"messageType": "Error", "text": "the value does not fit the element"}]})
    self.answer(204)

  def do_PUT(self):
    # a whole element replaced (PutSubmodelElementByPath) - a SubmodelElementList, its items Properties that fit
    raw = self.body()
    if not self.authorized():
      return
    p = self.parts()
    if len(p) != 4 or p[0] != "submodels" or p[2] != "submodel-elements":
      return self.notFound()
    with lock:
      sm = byId(SUBMODELS, b64decode(p[1]))
      e = element(sm, p[3]) if sm is not None else None
      if e is None:
        return self.notFound()
      try:
        new = json.loads(raw)
      except ValueError:
        new = None
      items = new.get("value") if isinstance(new, dict) and new.get("modelType") == "SubmodelElementList" else None
      self.logLine("PUT %s.%s %s" % (sm["idShort"], urllib.parse.unquote(p[3]),
                                     json.dumps([x.get("value") for x in items], separators=(",", ":")) if isinstance(items, list) else raw.decode()))
      ok = (e["modelType"] == "SubmodelElementList" and isinstance(items, list) and new.get("idShort") == e.get("idShort") and
            all(isinstance(x, dict) and x.get("modelType") == "Property" and isinstance(x.get("value"), str) and
                fromText({"modelType": "Property", "valueType": x.get("valueType", "xs:string")}, x["value"]) is not None for x in items))
      if ok:
        e.clear()
        e.update(new)
    if not ok:
      return self.answer(400, {"messages": [{"messageType": "Error", "text": "the element does not fit"}]})
    self.answer(204)

  def token(self, raw):
    # the client credentials grant: the client by HTTP Basic, its id and secret form-encoded (RFC 6749 2.3.1)
    given = self.headers.get("Authorization") or ""
    form = urllib.parse.parse_qs(raw.decode())
    try:
      id, _, secret = base64.b64decode(given[6:]).decode().partition(":")
      client = urllib.parse.unquote_plus(id) + ":" + urllib.parse.unquote_plus(secret)
    except Exception:
      client = None
    if not given.startswith("Basic ") or client != args.oauth2:
      return self.answer(401, {"error": "invalid_client"})
    if form.get("grant_type") != ["client_credentials"]:
      return self.answer(400, {"error": "unsupported_grant_type"})
    with lock:
      n = len(tokens) + 1
      tokens["tok-%d" % n] = time.time() + args.token_life
    self.logLine("token %d scope=%s" % (n, form.get("scope", ["-"])[0]))
    self.answer(200, {"access_token": "tok-%d" % n, "token_type": "Bearer", "expires_in": args.token_life})

  def do_POST(self):
    raw = self.body()
    p = self.parts()

    if p == ["token"] and args.oauth2 is not None:
      return self.token(raw)

    if p == ["test", "revoke"]:
      with lock:
        for t in tokens:
          tokens[t] = 0
      return self.answer(200, {})

    if p[0] == "test":
      req = json.loads(raw)
      with lock:
        sm = byId(SUBMODELS, req["submodel"])
        if p == ["test", "set"]:
          element(sm, req["path"])["value"] = req["value"]
        elif p == ["test", "add"]:
          sm["submodelElements"].append(req["element"])
        elif p == ["test", "remove"]:
          sm["submodelElements"] = [e for e in sm["submodelElements"] if e.get("idShort") != req["idShort"]]
      return self.answer(200, {})

    if not self.authorized():
      return
    if len(p) != 5 or p[0] != "submodels" or p[2] != "submodel-elements" or p[4] != "invoke":
      return self.notFound()
    with lock:
      sm = byId(SUBMODELS, b64decode(p[1]))
      e = element(sm, p[3]) if sm is not None else None
      if e is None or e["modelType"] != "Operation":
        return self.notFound()
      req = json.loads(raw)
      inputs = {a["value"]["idShort"]: a["value"]["value"] for a in req.get("inputArguments", [])}
      self.logLine(" ".join(["invoke %s.%s" % (sm["idShort"], urllib.parse.unquote(p[3]))] +
                            ["%s=%s" % (k, v) for k, v in inputs.items()]))
      result = operation(e["idShort"], inputs)
    self.answer(200, result)


def main():
  global args
  p = argparse.ArgumentParser()
  p.add_argument("--port", type=int, required=True)
  p.add_argument("--log", required=True)
  p.add_argument("--authorization")
  p.add_argument("--values-as-text", action="store_true")
  p.add_argument("--cert")
  p.add_argument("--keyfile")
  p.add_argument("--client-ca")
  p.add_argument("--oauth2")
  p.add_argument("--token-life", type=int, default=60)
  args = p.parse_args()

  server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
  server.daemon_threads = True

  if args.cert:
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(args.cert, args.keyfile)
    if args.client_ca:
      ctx.verify_mode = ssl.CERT_REQUIRED
      ctx.load_verify_locations(args.client_ca)
    server.socket = ctx.wrap_socket(server.socket, server_side=True)

  server.serve_forever()


main()
