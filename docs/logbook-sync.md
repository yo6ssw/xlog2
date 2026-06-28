# Logbook sync

Peer-to-peer, multi-master synchronisation of the **default logbook**
(`$XDG_DATA_HOME/xlog2/default.xlog`) across machines. You may add, edit and
delete QSOs on any machine; the logbooks converge automatically on a LAN and
self-heal after an offline period. Conflicts resolve **last-write-wins (LWW)**.

Instances are **symmetric**: every node both listens and dials and discovers its
peers automatically — there is **no listener/connector role** to configure. This
is provided by the bundled **multimaster** LAN gossip mesh
(`third_party/multimaster`, a git submodule), which handles discovery,
connection, dial races, relaying and reconnection. xlog2 layers the sync
protocol and the merge on top.

This document is the reference for the design and the operator controls. Update
it when changing the sync code.

## What gets synced

Only the persistent default logbook. In-memory *New Tab* logbooks and other
opened files are never synced (they have no stable identity). Two or more
machines can participate (the protocol runs per peer); the common case is a pair.

## Identity & change tracking (storage)

`LogBook` carries three pieces of sync state, added by the schema migration on
open (older `.xlog` files are upgraded transparently):

- `qsos.uuid` — a stable cross-machine id. New QSOs get a random UUIDv4.
  Pre-existing rows are **back-filled deterministically** with a content-hash
  UUIDv5 over `call|band|mode|date|time_on|…`, so two machines that started from
  a copy of the same file assign the *same* uuid to the same QSO (the first sync
  is then a no-op rather than a flood of duplicates).
- `qsos.updated_at` — ISO-8601 UTC millisecond timestamp, bumped on every local
  write. Back-filled deterministically from the QSO's own date/time.
- `tombstones(uuid, deleted_at)` — a deleted QSO leaves a tombstone so the
  deletion propagates instead of the row silently reappearing from the peer.
- `meta.sync_id` — a per-logbook id agreed at first pairing; both peers refuse
  to merge if their `sync_id`s differ (guards against syncing unrelated logs).

## Merge semantics (LWW)

`LogBook::applyRemote` merges a remote delta in **one transaction + one reload**
(never per-record, which would be O(n²)). Per uuid:

- newer `updated_at` wins; on an exact tie the higher **node id** wins
  (deterministic on both peers);
- a tombstone whose `deleted_at` is newer-or-equal deletes (delete wins ties);
- an edit strictly newer than a delete **resurrects** the record (an edit is a
  write — LWW-consistent); the stale tombstone is then cleared.

Re-applying the same delta is idempotent (strict `>` comparisons), so the
real-time push and the periodic anti-entropy pass compose safely regardless of
message ordering.

## Transport & protocol

`LogbookSync` (a service) wraps a **multimaster mesh**: a node both listens and
dials, auto-discovers LAN peers via UDP multicast, resolves simultaneous-dial
races itself, relays, and reconnects — all on the mesh's own IO thread.
`LogbookSync` does nothing but translate between that thread and the UI thread
(marshalling via `IUiDispatcher`) and between mesh messages and `SyncProtocol`
frames. The mesh `group` is derived from the shared secret
(`syncproto::meshGroup`), so only same-secret instances ever form a mesh.
`SyncCoordinator` runs the protocol on the UI thread, **per peer**, and is the
only thing that touches the logbook. Optional **static peers** (host:port) cover
the WAN (internet) case where multicast can't reach.

### Transport security (PSK + identity)

When a shared secret is set, the secret doubles as the mesh **PSK**: the
multimaster transport (via libsodium) mutually authenticates every peer link and
**encrypts it** (X25519 key agreement → ChaCha20-Poly1305, per-connection forward
secrecy), and discovery announces carry a keyed MAC. On the wire the QSO data is
no longer plaintext — a tunnel is no longer required for confidentiality (though
it's still fine to use one).

On a secured mesh each node also holds a persistent **self-certifying Ed25519
identity**, seeded from `node_identity` (mode 0600) in the data dir. The node's
mesh id becomes a hash of its public key, so a peer's id *proves* ownership of
its key — peers can't impersonate one another, and `Reject peers without a
verified identity` (on by default) drops any peer that presents none.

### Per-node trust (app-layer allowlist)

Identity tells you *which* key a peer holds; **trust** decides who you actually
sync with. Rather than the mesh's own allowlist (which would reject unknown peers
at the handshake, so you'd never see them), xlog2 enforces trust one layer up, in
`SyncCoordinator`, keyed by mesh id: the mesh admits every identity-verified peer,
but an **untrusted** peer gets no `HELLO` and none of its frames are processed —
it simply surfaces in **Sync ▸ Trusted peers** for the operator to admit. QRZ
peer-cache queries are gated the same way. `SyncCoordinator::setTrust` seeds the
allowlist from settings; `trustPeer`/`revokePeer` mutate it live and the change is
persisted. `xlog2-syncd`, being a headless backup peer, does **not** enforce trust
— it accepts any same-secret, identity-verified node and prints its own identity
key so you can trust it from a GUI peer.

Frames: `magic 'XLSG' | version | type | length | payload` (`SyncProtocol`); the
mesh delivers each as one complete message. When a peer becomes reachable, both
sides exchange `HELLO`/`HELLO_ACK`, authenticating with an HMAC over a nonce
keyed by the shared secret and agreeing the `sync_id`. Then the higher-node-id
side sends a `DIGEST`; on mismatch the other returns its `MANIFEST`; the
initiator pulls what it lacks (`REQUEST` → `RECORDS`), pushes what it has newer
(`RECORDS`), and applies tombstones straight from the manifest. Steady state:
each local add/edit/delete is pushed (`PUSH`) to every **authenticated** peer
(never to a peer that failed the secret/sync_id gate). All of this runs
independently per peer, so any number of peers converge.

## Operator setup

Edit ▸ Settings ▸ **Sync** (both frontends), or the **Sync** menu:

- **Enabled** — turn sync on/off (also a menu toggle).
- **Shared secret** — the one knob that matters: give every machine the **same**
  secret. It authenticates peers (HMAC handshake), **encrypts** every link (PSK),
  selects the mesh (group is derived from it) and enables per-node identity — so
  different-secret instances never even connect. Use a high-entropy value (it is
  hashed to a key, not stretched — not a weak password).
- **Node name** — optional signed display name gossiped to peers (advisory; shown
  in the Trusted-peers dialog). The mesh id stays the canonical identifier.
- **Reject peers without a verified identity** — on by default; leave it on unless
  bridging a transitional mesh that still has legacy (pre-identity) nodes.
- **Listen port** — the mesh's TCP port (default 7388). Peers on a LAN don't need
  it to match (they advertise their port via multicast), but a fixed value is
  needed for a WAN peer to reach this node.
- **WAN peer / WAN peer 2** — optional host(s) to dial persistently for syncing
  over the internet (where multicast doesn't reach). Left blank for LAN-only.

There is **no role to set** — start the app on each machine with the same secret
and they find each other. The status bar shows `⇄ N` (reachable peers).
**Sync now** forces an anti-entropy pass with every connected peer.

### Trusting peers

**Sync ▸ Trusted peers…** opens a live dialog: it shows this node's identity key
(to share), the peers you already trust (with an online indicator and *Revoke*),
and any **discovered-but-untrusted** peers currently connected (with *Trust*). A
peer exchanges logbook data only once trusted — and trust is per-side, so each
operator admits the other. The list updates as peers come and go.

### Networking

On a LAN, auto-discovery (UDP multicast, local subnet) needs nothing configured.
Over the internet, set one reachable side as the other's **WAN peer** (open /
port-forward the listen port). With a shared secret set, the link is already
authenticated **and encrypted** (PSK), so a tunnel is no longer required for
confidentiality — though running both inside a **WireGuard/SSH tunnel** remains a
fine way to avoid exposing the port at all.

## Assumptions & limitations

- **Clocks**: LWW relies on machines having reasonably correct UTC clocks (run
  NTP). The scheme is wall-clock + node-id tiebreak rather than a hybrid logical
  clock — far simpler for a single-operator setup; an HLC is the clean upgrade
  path if multi-second skew becomes a problem. The node id is the mesh peer id,
  minted once and persisted (`[sync] node_id`).
- **Multicast**: LAN discovery uses multicast on the local subnet (TTL 1). If the
  network blocks it (some Wi-Fi APs, containers), use a WAN-peer entry pointing at
  the other host instead.
- Tombstones are kept indefinitely (tiny); age-based GC is a TODO.
- Bulk operations (DXCC fill, locator fill, LoTW mark) propagate per affected
  row; LoTW *confirmations* propagate on the next anti-entropy pass.
