# corAasBridge

**Prototype - not released.** An Asset Administration Shell (AAS) server in the coraine NGSI-LD broker: a
shell, its submodels and their elements as **AssetChannel** entities, a written attribute as the element's
new value, and an Operation as a service the broker executes - `aas.so`, a bridge plugin loaded with
`--bridges aas`. It speaks the AAS HTTP/REST API (IDTA Part 2, V3) as any AAS client does.

## AssetChannels

An AssetChannel's endpoint is a **shell's id** - or a submodel's. The plugin reads its model and hands it to
the broker as the neutral node tree (coraine `doc/asset-channels.md` § 3.3); the AssetChannel's mapping
(`entities`, `attributeLayout`) makes the entities - the plugin maps nothing. With `"entities": "natural"`
the shell and each submodel are an entity; the shell's submodels are Relationships of the shell.

**The read:** `GET /shells/{id}` (none: `GET /submodels/{id}`), then each submodel the shell refers to,
`GET /submodels/{id}` - each id base64url-encoded, no padding. A submodel the server does not have is no
part of the model.

| AAS | the node tree | NGSI-LD (coraine's mapping) |
|---|---|---|
| Shell | `shell`, its `assetInformation`'s `globalAssetId` and `assetKind` as two leaves | an entity, type `Shell` |
| Submodel | `submodel`, `typeName` its idShort | an entity, type its idShort |
| Property | a leaf, the value converted by its `valueType` - `xs:int` ... a number, `xs:double` ... a number, `xs:boolean` a boolean, the rest a string | Property |
| MultiLanguageProperty | a leaf, `attributeType` LanguageProperty, `{ "<language>": text }` | LanguageProperty |
| Range | a leaf, `{ "min", "max" }` converted | Property |
| File | a leaf: its URL - a path on the server (a file of the AASX package) the API's attachment URL - and `contentType` | Property, a `contentType` sub-Property |
| Blob | a leaf: the API's attachment URL, and `contentType` - the content is not copied | Property, a `contentType` sub-Property |
| ReferenceElement | a reference of the node it is in: its idShort, its target the id of the node it refers to - the reference's key; more keys (an element): `<submodel id>#<idShortPath>`, a list's item by its index (`Stations[1]`) | Relationship to the entity the node is or is in - of this AssetChannel or another one; to an element with a `targetNode` sub-Property, its id; else Property |
| RelationshipElement | a reference of the node `first` names (the shell, a submodel, an element): its idShort, its target what `second` refers to, as a ReferenceElement's; a `first` not in the model read (another shell's node, an external reference), or no `second`: nothing, `first`'s case logged | as a ReferenceElement's, on the entity `first` is or is in |
| SubmodelElementCollection | `collection`, its id `<submodel id>#<idShortPath>` | by `attributeLayout` |
| SubmodelElementList | of Properties (declared so, or of any element - `SubmodelElement`, `DataElement` - and every item a Property of one valueType): a leaf, `attributeType` ListProperty; of references: a reference each; of anything else: a collection, its items named by their index | ListProperty, ListRelationship, by `attributeLayout` |
| Operation | `operation`, its `input` and `output` variables, and `endpoint` - where the bridge invokes it (below) | no attribute - a Service Registration of its entity, that endpoint |
| semanticId | `semanticIds` (the reference's first key) | a `semanticId` sub-Property |

Entity, AnnotatedRelationshipElement, Capability and BasicEventElement are not mapped,
nor is a Property without a value.

A leaf's id is `<submodel id>#<idShortPath>` (`AddressInformation.Street`, `Markings[0]`).

**The model read again** every `pollInterval` (`channelInfo`, an ISO 8601 duration - `PT5S`, `PT0.5S`; 10 s
by default, 0.1 s at the least), one channel at a time: a value that changed is handed over as
`{ "values": [ ... ] }`, its `sourceTime` (the attribute's `observedAt`) the time of the read; a change of
the structure (an element added or removed, a submodel gone) hands the model over again - what the model
no longer has is left alone in the broker. A server that does not answer, or has no such shell, makes the
channel dormant, with why (`GET /ngsi-ld/v1/assetChannels`); read again, it is available.

**The payload check** (bridge ABI 13): a client's write to an attribute made from a leaf is refused when it
does not fit the element - a string to an `xs:double`, 3000000000 to an `xs:int`: 400, nothing stored. On a
channel that is inbound only, too.

A Channel or a DataChannel configured for this bridge is refused (dormant): it carries AssetChannels only.

### Write-back

With `"channelDirection": "both"` (coraine's AssetChannel write-back), a client's write to an attribute
made from a leaf goes to the server first - the value, as the application's JSON, in the element's ValueOnly
form:

    PATCH /submodels/{id}/submodel-elements/{idShortPath}/$value

| element | the body |
|---|---|
| Property | the value: `300`, `"manual"`, `true` |
| MultiLanguageProperty | `[ { "en": "ACME" }, { "de": "ACME DE" } ]` - from the LanguageProperty's languageMap |
| Range | `{ "min": 10, "max": 350 }` |
| SubmodelElementList of Properties | not by `$value`: the element read (`GET .../submodel-elements/{idShortPath}`), its items replaced - each a copy of the first (its `valueType`, `semanticId`), its value one of the ListProperty's valueList, a string as the AAS JSON has it - and written whole, `PUT .../submodel-elements/{idShortPath}` |

- **Numbers and booleans as strings.** IDTA-01001's ValueOnly has an `xs:int`, `xs:double`, ... as a JSON
  number and an `xs:boolean` as a JSON boolean, and the bridge writes them so. Some servers take them only as
  strings (`"421"`, `"true"`) - Eclipse BaSyx answers 500 to `421` ([#384](https://github.com/eclipse-basyx/basyx-java-server-sdk/issues/384),
  [#403](https://github.com/eclipse-basyx/basyx-java-server-sdk/issues/403)). A write refused with a number or a
  boolean in it (any 4xx or 5xx but 401, 403 and 404) is sent once more with them as strings; taken, every
  later write to the server goes out so at once, and the log says it, once. A LanguageProperty has no number:
  it is written once.
- **A list is written whole**, not by its `$value`: Eclipse BaSyx does not implement `$value` on a
  SubmodelElementList (501), and FA³ST Service cannot write a list of any element so (500) and, when the number
  of items changes, answers 204 and keeps the items it had. A PUT of the element is taken by both.
- A File, a Blob and the shell's assetInformation are not written back: 422.
- The write is queued and the client answered at once; a writer thread sends the writes one at a time. Up
  to 256 wait; a full queue is refused (503), as is a write before the server has ever answered.
- A write the server refuses (400, unreachable) is logged, and the element is read back at once: the
  entity shows what the server has, not what was asked of it.
- The server's next read of a written value is no change: a write is not handed back in.

## Operations - Service Execution

An Operation is a service the bridge executes (coraine `doc/service-execution.md`, bridge ABI 11): a
Service Registration whose endpoint is

    aas+http[s]://<host>[:<port>][<base path>]/submodels/<submodel id, base64url>/submodel-elements/<idShortPath>

is the Operation. `POST /ngsi-ld/v1/entities/{id}/services/{name}` (or `POST /services`) then:

1. `GET` the Operation - its input, output and inoutput variables;
2. the execution input made its input arguments by idShort, each converted to the variable's `valueType`
   (the entity's `id` and `type`, which the broker adds, are left out). A member that is no variable, a
   variable the input does not have, a value that does not fit: the execution fails, nothing invoked;
3. `POST .../invoke` (synchronous, `clientTimeoutDuration` PT60S);
4. the OperationResult's output (and inoutput) arguments by idShort, converted by their `valueType`, are the
   execution's output. `success: false`, or an `executionState` other than `Completed`, fails it - its first
   message the reason.

A failed execution's ProblemDetails: status 502, title "The AAS operation failed", the detail why. Only
Property variables are taken. An invocation on the configured server carries its Authorization header; one
on another host does not. One invocation at a time, on a thread of its own; an invocation cannot be
cancelled.

## Inventory

`GET /admin/bridges/aas/inventory` (bridge ABI 14) - for a mapping tool:

- no `from`: the server's shells (`id`, `idShort`, their `submodels`) and submodels (`id`, `idShort`,
  `semanticId`), every page of `GET /shells` and `GET /submodels`;
- `?from=<id>`: that shell's (or submodel's) model, the node tree as it is handed over - `depth` is not
  used: a model is read whole. 404: the server has no such shell or submodel.

## Configuration

```json
{
  "aas": {
    "server": { "url": "https://aas.acme.example/api/v3.0", "authorizationEnv": "AAS_AUTHORIZATION",
                "caFile": "/etc/coraine/aas-ca.pem" },
    "ngsild": {
      "defaultEntity": false,
      "assetChannels": {
        "https://acme.example/ids/aas/press-17": {
          "entities": "natural",
          "channelDirection": "both",
          "channelInfo": [ { "key": "pollInterval", "value": "PT2S" } ] } }
    }
  }
}
```

| `server` member | |
|---|---|
| `url` | `http[s]://<host>[:<port>][/<base path>]` - where the API is (`/api/v3.0` on some servers) |
| `authorizationEnv` | the environment variable that holds the `Authorization` header's value (`Bearer ...`) - a credential is never in the file. Set and absent: the plugin is not loaded |
| `caFile` | the PEM of the CAs the server's certificate is verified against. Absent: the system's CA certificates |
| `insecure` | `true`: the certificate is not verified |
| `clientCert` | the PEM of the plugin's own certificate (and its chain), for a server that asks for one - mutual TLS. Also shown to the OAuth2 token endpoint, and to an Operation's server when it is this one |
| `clientKey` | its private key, PEM, not encrypted. Absent: in `clientCert`'s file |
| `oauth2` | `{ "tokenUrl", "clientId", "clientSecretEnv", "scope" }` - an OAuth2 access token instead of `authorizationEnv`: the client credentials grant (RFC 6749, 4.4), the client's id and secret sent as HTTP Basic, the secret from the environment variable `clientSecretEnv` names; `scope` optional |

The certificate must be issued to the url's host. The plugin's TLS is its own (OpenSSL, a context of its
own): the broker's TLS settings are not touched.

**The OAuth2 token** is fetched when the first request needs it and sent as `Authorization: Bearer <token>`.
It is one token for the plugin, shared by its threads. It is fetched again a fifth of its life before it
expires (at most 30 s before; no `expires_in`, only when refused), and when a request is answered 401 - that
request is then sent once more, with the new token. No token to be had (the endpoint refuses, or does not
answer): the AssetChannel is dormant, the reason says why. `caFile` and `insecure` hold for the token
endpoint too.

```json
"server": { "url": "https://aas.acme.example/api/v3.0", "caFile": "/etc/coraine/aas-ca.pem",
            "oauth2": { "tokenUrl": "https://login.acme.example/realms/aas/protocol/openid-connect/token",
                        "clientId": "coraine", "clientSecretEnv": "AAS_CLIENT_SECRET", "scope": "aas.read" } }
```

**A registry.** With `"registry": { "url": "http[s]://<host>[:<port>][/<base path>]" }` (a member of `aas`, beside
`server`), an AssetChannel's shell is looked up first at an AAS registry (IDTA-01002, `GET /shell-descriptors/{id}`):

- the shell is read from its descriptor's endpoint (the `AAS-3.0` interface's, else the first). The `href` may be the
  shell's own URL (`.../shells/{id}`, as Eclipse BaSyx registers it) or the server's base URL;
- each of its submodels is read from its entry's endpoint in the descriptor's `submodelDescriptors` (the
  `SUBMODEL-3.0` interface's); a submodel not among them, from the shell's server;
- a shell the registry does not know (404), and a submodel as an AssetChannel's target, are read from `server.url`;
- a write goes to the server of the element's submodel. The registry is asked on every read, so a submodel it
  moves is read, and written, at its new place from the next poll on.

The credentials (`authorizationEnv`, the OAuth2 token) and `clientCert` go only to `server.url`'s host - the same
scheme, host and port: to the registry and to the servers it names only when they are that host. Every other server
gets the request without them, verified against `caFile` (or `insecure`) all the same.

```json
"aas": { "server": { "url": "https://aas.acme.example/api/v3.0", "authorizationEnv": "AAS_AUTHORIZATION" },
         "registry": { "url": "https://aas.acme.example/registry/api/v3.0" }, "ngsild": { ... } }
```

## Build and test

    make && make test          # libssl-dev; the tests run python3 and the openssl tool

`aas_basyx` runs against a real AAS server, Eclipse BaSyx's AAS environment (2.0.0-milestone-16): corTest's
`-basyx` - an image the test starts in docker (the default when docker answers), the URL of one running (CI's
service, `AAS_BASYX_URL`), or `off` (the test is skipped). It POSTs its own shell and submodel; an Operation BaSyx
delegates goes to `test/funcTests/basyxDelegate.py`. A list is written by a PUT of the element (Write-back, above).

`aas_faaast` runs against a second real server, FA³ST Service 1.3 (Fraunhofer IOSB): corTest's `-faaast`, as `-basyx`
(CI's service, `AAS_FAAAST_URL`), with FA³ST's defaults - https with a self-signed certificate (`insecure`). FA³ST takes
ValueOnly numbers and booleans as JSON, no fallback needed. It runs an Operation only through an asset connection of
its own configuration - without one it answers 500, the client 502 with its reason. A list is written by a PUT of the
element (Write-back, above).

The other tests run on `test/funcTests/aasMock.py`, which plays the server: a shell with a Digital Nameplate and an operational-data
submodel, PATCHes and invocations logged, a value that does not fit refused, three Operations, https with a
certificate made by the test, and an Authorization header; `--values-as-text` makes it take numbers and
booleans as strings only, as BaSyx does; `--oauth2` makes it a token endpoint too, `--client-ca` takes only a client
with a certificate (`aas_oauth2`, `aas_mtls`). It is a registry too (`/registry/shell-descriptors`, the descriptors the
test gives), and many servers by base path (`/srv-<x>/...`); `--log-requests` logs each request's host and whether
credentials came with it (`aas_registry`).

The broker must have AssetChannels; write-back (`COR_FEATURE_CHANNEL_WRITE_BACK`) for `"both"`, and Service
Execution for the Operations.
