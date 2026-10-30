# LAN multicast discovery: requirements & troubleshooting

xlog2's sync mesh (and `xlog2-syncd`) finds peers on the LAN by **UDP
multicast**; no addresses need configuring. This page lists what the network and
each host must allow for that to work, and how to find out which link is broken
when it doesn't. For the sync protocol itself see [`logbook-sync.md`](logbook-sync.md).

## How discovery works

| | |
|---|---|
| Group / port | `239.255.42.99`, UDP `45454` |
| TTL | 1 (local subnet only, by design) |
| Announce rate | ~1 per second per node (±20% jitter) |
| Payload | protocol version, node id, mesh group, **TCP listen port**; with a shared secret, a keyed MAC so only same-secret nodes accept it |
| Connection | TCP to the announced port (default `7388`); encrypted with the shared secret |

Each node sends announcements and listens for other nodes' announcements. When a
node hears a same-group announcement from a peer it isn't connected to, it
**dials that peer over TCP**. So for two nodes to connect it is enough that
**one** of them hears the other **and** can open a TCP connection to it.

Nodes do **not** exchange peer addresses over the mesh: the only ways a node
learns a LAN address are a multicast announcement or a configured static peer.

## Checklist

Every node that should be discoverable needs all of the following.

### 1. Firewall: accept announcements, connections and IGMP

From the LAN subnet, allow:

- **UDP 45454** to `239.255.42.99` (the announcements),
- **TCP on the listen port** (`[sync] port`, default `7388`; for `xlog2-syncd`
  whatever `--port` is set to),
- **IGMP (IP protocol 2)**. This one is easy to miss, and its symptoms are
  confusing; see [IGMP snooping](#2-igmp-snooping-the-host-must-answer-queries)
  below.

**ufw** (Ubuntu's default). Ports go in with `ufw allow`, but `ufw allow` can't
express protocol 2, so IGMP goes in `/etc/ufw/before.rules`, inside the
`*filter` section, before its `COMMIT`:

```sh
sudo ufw allow from 192.168.1.0/24 to any port 45454 proto udp comment 'xlog2 sync discovery'
sudo ufw allow from 192.168.1.0/24 to any port 7388  proto tcp comment 'xlog2 sync'
```
```
# /etc/ufw/before.rules — before the first COMMIT
# allow IGMP membership queries from the LAN router (multicast snooping)
-A ufw-before-input -p igmp -s 192.168.1.0/24 -j ACCEPT
```
```sh
sudo ufw reload
sudo iptables -S ufw-before-input | grep -- ' -p 2 '   # should list the rule
```

(Replace `192.168.1.0/24` with your LAN.) ufw's stock rules allow multicast only
for mDNS (5353) and SSDP (1900), so out of the box ufw drops all three.

**firewalld:**
```sh
sudo firewall-cmd --permanent --add-port=45454/udp --add-port=7388/tcp
sudo firewall-cmd --permanent --add-protocol=igmp
sudo firewall-cmd --reload
```

**Plain nftables:** accept `ip protocol igmp`, `udp dport 45454` and
`tcp dport 7388` in the input chain.

### 2. IGMP snooping: the host must answer queries

Most home routers, managed switches and Wi-Fi APs (and the Linux bridge, e.g.
Proxmox's `vmbr0`) do **IGMP snooping**: they forward a multicast group only to
ports or Wi-Fi clients that have recently reported membership of it. The
mechanism:

1. When a program joins the group, the host sends **one** membership report.
2. A **querier** (usually the router) sends an IGMP query every ~125 s.
3. Every member host answers with a fresh report. A membership that isn't
   refreshed **expires after ~260 s**, and forwarding to that host stops.

If the host's firewall drops the queries (step 2), the host never refreshes its
membership, and you get the classic symptom:

> Discovery works for a few minutes after starting xlog2 / `xlog2-syncd`, then
> stops. New peers are found only right after a restart. Other devices, such as
> a phone, keep working fine.

(Phones work because Android has no host firewall dropping the queries, and the
app re-joins whenever Wi-Fi reconnects or the app resumes.)

The fix is the IGMP allow rule from step 1. If the network has **no querier at
all** (check with `tcpdump -ni <iface> igmp` for a couple of minutes; you should
see `igmp query` from the router), either enable one on the router ("IGMP
proxy"/"querier") or on a Linux bridge:

```sh
echo 1 > /sys/class/net/vmbr0/bridge/multicast_querier      # live
# persistent (ifupdown2 / Proxmox): under "iface vmbr0" in /etc/network/interfaces
#   bridge-mcquerier 1
```

Only do this when there's no querier; one is enough.

### 3. Wi-Fi access point

Wi-Fi APs can drop or mangle multicast between wireless and wired clients, or
between wireless clients. If the hosts are fine (steps 1–2) but nothing crosses
the AP, check its settings for:

- **AP / client isolation**: must be off.
- **IGMP snooping** without a working querier (see above).
- **"Multicast to unicast" / "multicast enhancement"**: usually fine when IGMP
  works, but it forwards only groups it has seen reports for. Try turning it off
  if in doubt.

### 4. Virtual machines and containers

- **VMs (Proxmox/KVM, bridged NIC)** work, but the hypervisor's bridge snoops
  IGMP too, and the VM's own firewall (and Proxmox's per-VM firewall, if it has
  rules) must pass IGMP + UDP 45454.
- **Docker / Podman** with default bridge networking won't see LAN multicast
  (TTL 1, and the container sits behind NAT). Run discovery nodes with **host
  networking**, or use static peers.
- Multiple nodes **on one host** work (the socket uses `SO_REUSEPORT` and
  multicast loopback), as long as they use different TCP ports.

### 5. Android

The app holds a Wi-Fi **MulticastLock** while sync is enabled (in its foreground
`SyncService`); without it Android silently drops incoming multicast. If an
Android node can't hear others, make sure the sync service is running and not
being killed by battery optimisation. Android can't resolve `*.local` names, so
use IP addresses for any static peer.

## Diagnosing step by step

Work through these on each host. The answer is usually obvious once you see
where the packets stop.

**Is the node up and listening?**
```sh
ss -tulnp | grep -E '45454|7388'      # UDP 45454 bound + TCP listen port
ip maddr show dev <iface> | grep 239.255.42.99    # group joined on the LAN NIC
ip route get 239.255.42.99             # multicast leaves via the LAN NIC (not docker0/virbr0)
```

**Are announcements arriving at the NIC?** `tcpdump` sees packets before the
firewall, so this tells you whether the *network* delivers them:
```sh
sudo tcpdump -ni <iface> 'udp port 45454 and not src host <this-host-ip>'
```
Expect roughly one packet per second from each other node. Nothing arriving
means the problem is upstream (the other host's membership, a switch, the AP or
the bridge). Packets arriving but no connection means this host's firewall, or
a secret/group mismatch.

**Is the firewall dropping them?**
```sh
sudo journalctl -k --since -1h | grep 'UFW BLOCK' | grep -E 'DPT=(45454|7388) |PROTO=2 '
```
`PROTO=2` lines from the router to `224.0.0.1` are dropped IGMP queries, which
is the snooping problem from step 2.

**Does the host answer IGMP queries?** Watch for one full query cycle (~2 min):
```sh
sudo tcpdump -ni <iface> igmp
```
You want to see `igmp query` from the router followed, within ~10 s, by
`igmp v2 report 239.255.42.99` from this host. Also check the kernel's view:
```sh
grep -A1 <iface> /proc/net/igmp | head -1    # Querier column
```
If the network's querier speaks IGMPv2, the column should read `V2` once a query
has been processed. If it stays at `V3` after a v2 query arrived, the query never
reached the IGMP stack, so the firewall is dropping it.

**Follow the path hop by hop.** On a hypervisor, capture on the physical uplink,
the VM's firewall-bridge port and its tap interface (Proxmox: `enp…`,
`fwpr<vmid>p0`, `tap<vmid>i0`) to see where packets stop.
`bridge -d mdb show dev vmbr0` lists the groups the bridge currently forwards
per port. An empty list for `239.255.42.99` means no member has reported to the
bridge.

### Pitfall: a test listener doesn't send a fresh report

A second socket joining a group the host **already** belongs to (for example a
quick Python listener while xlog2 is running) sends **no** new IGMP report. So
it can't revive an expired membership, and it hears nothing, just like the real
node. That makes such a test look like "multicast is completely broken" when the
real problem is the expired membership. Restarting the node (which leaves and
re-joins) does send a report, so discovery working briefly after a restart is a
strong hint that you have the snooping problem.

## Fallback: static peers

If multicast can't be made to work, discovery isn't required. Point one node at
the other explicitly:

- GUI: Edit ▸ Settings ▸ Sync ▸ **WAN peer** (`[sync] peer_host`) =
  `host:port`, e.g. `192.168.1.41:7388`.
- `xlog2-syncd`: run it with a fixed `--port` so others can dial it.

A static peer is dialed persistently and reconnected with backoff. The dialing
side only needs outbound TCP, so its own firewall doesn't matter, but the target
must accept the TCP port.
