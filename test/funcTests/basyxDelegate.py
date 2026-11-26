#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# basyxDelegate.py - what Eclipse BaSyx invokes for an Operation it delegates (the Operation's qualifier
# invocationDelegation): POST /<operation> with the input variables, [ { "value": <Property> } ], answered with
# the output variables. Each invocation is logged, one line: "<operation> <idShort>=<value> ...".
#
#   scale   Factor (xs:double) -> Result (xs:double), Factor * 2
#
#   basyxDelegate.py --host <address> --port <p> --log <file>
#
import argparse
import json
from http.server import BaseHTTPRequestHandler, HTTPServer

args = None


class Handler(BaseHTTPRequestHandler):
  def log_message(self, fmt, *a):
    pass

  def do_POST(self):
    n = int(self.headers.get("Content-Length", "0"))
    inputs = {v["value"]["idShort"]: v["value"].get("value") for v in json.loads(self.rfile.read(n) or b"[]")}
    name = self.path.strip("/")
    with open(args.log, "a") as f:
      f.write(" ".join([name] + ["%s=%s" % (k, v) for k, v in sorted(inputs.items())]) + "\n")

    if name != "scale":
      self.send_response(404)
      self.send_header("Content-Length", "0")
      self.end_headers()
      return

    out = [{"value": {"modelType": "Property", "idShort": "Result", "valueType": "xs:double",
                      "value": repr(float(inputs["Factor"]) * 2)}}]
    body = json.dumps(out).encode()
    self.send_response(200)
    self.send_header("Content-Type", "application/json")
    self.send_header("Content-Length", str(len(body)))
    self.end_headers()
    self.wfile.write(body)


def main():
  global args
  p = argparse.ArgumentParser()
  p.add_argument("--host", required=True)
  p.add_argument("--port", type=int, required=True)
  p.add_argument("--log", required=True)
  args = p.parse_args()
  HTTPServer((args.host, args.port), Handler).serve_forever()


main()
