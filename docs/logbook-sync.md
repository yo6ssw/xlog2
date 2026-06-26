# Logbook sync

Peer-to-peer, multi-master synchronisation of the **default logbook**
(`$XDG_DATA_HOME/xlog2/default.xlog`) across two machines. You may add, edit and
delete QSOs on either machine; the logbooks converge automatically on a LAN and
self-heal after an offline period. Conflicts resolve **last-write-wins (LWW)**.

This document is the reference for the design and the operator controls. Update
it when changing the sync code.

## What gets synced

Only the persistent default logbook. In-memory *New Tab* logbooks and other
opened files are never synced (they have no stable identity). v1 supports a
**pair** of machines (one peer at a time).

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

`LogbookSync` (a service) owns a TCP socket on a worker thread and does *only*
I/O + framing; `SyncCoordinator` runs the protocol on the UI thread and is the
only thing that touches the logbook. One peer **Listens**, the other
**Connects** (with auto-reconnect). Keepalive Ping/Pong is handled in the
transport.

Frames: `magic 'XLSG' | version | type | length | payload` (`SyncProtocol`).
Handshake `HELLO`/`HELLO_ACK` authenticates with an HMAC over a nonce keyed by
the shared secret, and agrees the `sync_id`. Reconcile: the higher-node-id peer
sends a `DIGEST`; on mismatch the other sends its `MANIFEST`; the initiator
pulls what it lacks (`REQUEST` → `RECORDS`), pushes what it has newer
(`RECORDS`), and applies tombstones straight from the manifest. Steady state:
each local add/edit/delete is pushed immediately (`PUSH`).

## Operator setup

Edit ▸ Settings ▸ **Sync** (both frontends), or the **Sync** menu:

- **Enabled** — turn sync on/off (also a menu toggle).
- **Role** — one machine *Listen*, the other *Connect*.
- **Peer host** / **Fallback host** — for the Connect role: the listener's
  address; the fallback is tried if the primary is unreachable (e.g. a LAN IP
  primary and an internet hostname fallback).
- **Port** — must match on both machines (default 7388).
- **Shared secret** — must match on both machines; authenticates the peer.

The status bar shows `⇄ waiting` / `⇄ peer`. **Sync now** forces an
anti-entropy pass.

### Networking

On a LAN, point the Connect machine at the Listen machine's IP. Over the
internet the listener must be reachable on the port (port-forward) — or, better,
run both inside a **WireGuard/SSH tunnel** and sync over that. The shared-secret
HMAC stops an unwanted peer from merging, but the QSO data on the wire is **not
encrypted**; tunnel it for confidentiality. TLS is a possible follow-up.

## Assumptions & limitations

- **Clocks**: LWW relies on both machines having reasonably correct UTC clocks
  (run NTP). The chosen scheme is wall-clock + node-id tiebreak rather than a
  hybrid logical clock — far simpler for a two-node, single-operator setup; a
  HLC is the clean upgrade path if multi-second skew becomes a problem.
- **Two peers** in v1 (one connection at a time).
- Tombstones are kept indefinitely (tiny); age-based GC is a TODO.
- Bulk operations (DXCC fill, locator fill, LoTW mark) propagate per affected
  row; LoTW *confirmations* propagate on the next anti-entropy pass.
