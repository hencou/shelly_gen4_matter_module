#!/usr/bin/env python3
"""
Wipe stale ACL entries AND the binding table from a Matter node
(e.g. IKEA Kajplats lamp).

ACL part:
Keeps only admin entries (privilege=Administer + targets=null) — those
are the real controllers such as Home Assistant. All binding-targets
entries (entries with non-empty targets, e.g. {cluster:6, endpoint:1})
and stray/incomplete fabric entries are dropped.

Binding part:
The binding table (cluster 0x001E / 30, attribute Binding 0x0000) is
fully cleared. These are separate device-to-device links (e.g. a switch
bound to this lamp) and have nothing to do with controller access, so
this is safe to clean independently of the ACL cleanup.

Example usage:
    python3 wipe_acl_on_matter_device.py                          # connect via default URL
    python3 wipe_acl_on_matter_device.py --ws ws://192.168.1.10:5580/ws --node 3
    python3 wipe_acl_on_matter_device.py --dry-run                # only show, don't write

Dependencies:
    pip install websockets

Hostname / port:
- Home Assistant OS / Supervised with Matter Server add-on:
      ws://192.168.178.2:5580/ws       (default)
   or ws://<HA-IP>:5580/ws
- Python-matter-server standalone:
      ws://<host>:5580/ws
"""

import argparse
import asyncio
import json
import sys
from typing import Any

try:
    import websockets
except ImportError:
    sys.exit("Missing 'websockets'. Install with: pip install websockets")


# ACL attribute path: endpoint 0, cluster 0x1F (AccessControl), attribute 0 (acl)
ACL_PATH = "0/31/0"

# Binding attribute path template: <endpoint>, cluster 0x1E (Binding, decimal 30),
# attribute 0 (binding). The endpoint differs per device — for most
# lamps/switches this is endpoint 1.
BINDING_PATH_TMPL = "{endpoint}/30/0"

# Group Key Management cluster (0x003F / 63) lives on endpoint 0.
# GroupKeyMap (attribute 0) is the fabric-scoped list of {GroupId, GroupKeySetID}
# mappings — this is effectively "which groups is this node in, for our fabric".
# MaxGroupsPerFabric / MaxGroupKeysPerFabric tell us the device's hard limit,
# which is what "Resource exhausted" means the node has hit.
GROUP_KEY_MAP_PATH = "0/63/0"
MAX_GROUPS_PER_FABRIC_PATH = "0/63/2"
MAX_GROUP_KEYS_PER_FABRIC_PATH = "0/63/3"

# Groups cluster (0x0004 / 4) is per-endpoint and holds the device-local group
# membership table. There's no attribute to read that table directly; it's
# cleared with the RemoveAllGroups command instead.
GROUPS_CLUSTER_ID = 4


async def send_command(
    ws: "websockets.WebSocketClientProtocol",
    command: str,
    args: dict,
    message_id: str = "msg-1",
) -> dict:
    """Send a JSON command to matter-server and wait for matching response."""
    payload = {
        "message_id": message_id,
        "command": command,
        "args": args,
    }
    await ws.send(json.dumps(payload))
    # Keep reading messages until we see the matching response (ignore event broadcasts).
    while True:
        raw = await ws.recv()
        msg = json.loads(raw)
        if msg.get("message_id") == message_id:
            return msg


def _field(entry: dict, named: str, tlv_key: str) -> Any:
    """Read a field from an ACL entry — accepts both named (privilege)
    and integer-TLV (1) keys, depending on how matter-server serializes
    it in this version."""
    if named in entry:
        return entry[named]
    return entry.get(tlv_key)


def filter_admin_entries(acl: list) -> tuple[list, list]:
    """Split ACL into (keep, drop). Keep only entries with
    privilege==Administer (5) and targets==None — those are the real
    controllers. All other entries (binding-targets, partial fabric
    entries) are dropped."""
    keep, drop = [], []
    for e in acl:
        priv    = _field(e, "privilege", "1")
        targets = _field(e, "targets",   "4")
        if priv == 5 and targets in (None, []):
            keep.append(e)
        else:
            drop.append(e)
    return keep, drop


def _normalize_scalar(resp: dict, path: str):
    """Same idea as _normalize_result() but for single-value attributes
    (MaxGroupsPerFabric, MaxGroupKeysPerFabric), which some matter-server
    versions also wrap in a {attribute_path: value} dict."""
    result = resp.get("result")
    if isinstance(result, dict):
        if path in result:
            return result[path]
        return next(iter(result.values()), None)
    return result


def _normalize_result(resp: dict, path: str) -> list:
    """matter-server can return the result as a list OR as a dict
    ({attribute_path: value}), depending on the version. Normalize to a list."""
    result = resp.get("result")
    if isinstance(result, dict):
        if path in result:
            return result[path] or []
        return next(iter(result.values()), []) or []
    if isinstance(result, list):
        return result
    sys.exit(f"ERROR: unexpected result format: {result!r}")


async def discover_binding_endpoints(ws, node_id: int) -> list[int]:
    """Find which endpoint(s) actually expose the Binding cluster (0x1E / 30)
    on this node. Not every device has a Binding cluster, and if it does,
    it isn't always on endpoint 1 — so ask the server instead of guessing.
    Uses the 'get_node' command, which returns the full cached node data
    including an 'attributes' dict keyed by 'endpoint/cluster/attribute'."""
    resp = await send_command(ws, "get_node", {
        "node_id": node_id,
    }, message_id="get-node")

    if "error_code" in resp or "error" in resp:
        sys.exit(f"ERROR during get_node: {resp}")

    node_data = resp.get("result", {})
    attributes = node_data.get("attributes", {})

    endpoints = set()
    for key in attributes:
        parts = key.split("/")
        if len(parts) == 3 and parts[1] == "30":
            endpoints.add(int(parts[0]))

    return sorted(endpoints)


async def discover_group_endpoints(ws, node_id: int) -> list[int]:
    """Find which endpoint(s) expose the Groups cluster (0x0004 / 4) on this
    node, the same way discover_binding_endpoints() does for cluster 30."""
    resp = await send_command(ws, "get_node", {
        "node_id": node_id,
    }, message_id="get-node-groups")

    if "error_code" in resp or "error" in resp:
        sys.exit(f"ERROR during get_node: {resp}")

    node_data = resp.get("result", {})
    attributes = node_data.get("attributes", {})

    endpoints = set()
    for key in attributes:
        parts = key.split("/")
        if len(parts) == 3 and parts[1] == str(GROUPS_CLUSTER_ID):
            endpoints.add(int(parts[0]))

    return sorted(endpoints)


async def read_stored_keyset_ids(ws, node_id: int) -> list[int]:
    """Ask the node which GroupKeySets it actually has stored (this is a
    separate resource from GroupKeyMap: GroupKeyMap only maps GroupId ->
    GroupKeySetID, the keyset itself — the crypto material — takes up its
    own slot regardless of whether anything currently maps to it).
    Uses the KeySetReadAllIndices command on the Group Key Management
    cluster (endpoint 0, cluster 0x3F / 63)."""
    resp = await send_command(ws, "device_command", {
        "node_id": node_id,
        "endpoint_id": 0,
        "cluster_id": 63,
        "command_name": "KeySetReadAllIndices",
        "payload": {},
    }, message_id="keyset-read-all-indices")

    if "error_code" in resp or "error" in resp:
        sys.exit(f"ERROR during device_command (KeySetReadAllIndices): {resp}")

    result = resp.get("result") or {}
    # Field name casing can differ between matter-server versions.
    ids = result.get("groupKeySetIDs")
    if ids is None:
        ids = result.get("GroupKeySetIDs", [])
    return list(ids)


async def main() -> int:
    ap = argparse.ArgumentParser(
        description="Wipe stale ACL entries from a Matter node.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    ap.add_argument("--ws",   default="ws://192.168.178.2:5580/ws",
                    help="matter-server WebSocket URL")
    ap.add_argument("--node", type=int, default=3,
                    help="lamp node id")
    ap.add_argument("--endpoint", type=int, default=None,
                    help="endpoint that holds the binding table. "
                         "If omitted, it is auto-detected via get_node.")
    ap.add_argument("--dry-run", action="store_true",
                    help="only show what would be written, change nothing")
    ap.add_argument("--skip-groups", action="store_true",
                    help="skip the Group Key Management / Groups cleanup step")
    args = ap.parse_args()

    print(f"[*] Connecting to {args.ws} ...")
    try:
        ws = await websockets.connect(args.ws, max_size=2**22)
    except Exception as exc:
        sys.exit(f"ERROR: could not connect: {exc}")

    async with ws:
        # Server-info packet that matter-server sends right after connecting.
        try:
            info = json.loads(await asyncio.wait_for(ws.recv(), timeout=5))
            print(f"[*] matter-server: schema={info.get('schema_version')} "
                  f"sdk={info.get('sdk_version')} "
                  f"fabric_id={info.get('fabric_id')}")
        except asyncio.TimeoutError:
            print("[!] No server-info packet within 5s — continuing anyway")

        # ----- 1) Read current ACL ---------------------------------------
        print(f"\n[*] Reading ACL of node {args.node} (path {ACL_PATH}) ...")
        resp = await send_command(ws, "read_attribute", {
            "node_id": args.node,
            "attribute_path": ACL_PATH,
        }, message_id="read-acl")

        if "error_code" in resp or "error" in resp:
            sys.exit(f"ERROR during read_attribute: {resp}")

        current = _normalize_result(resp, ACL_PATH)

        print(f"[*] Current ACL ({len(current)} entries):")
        for i, e in enumerate(current):
            print(f"      [{i}] {e}")

        # ----- 2) Filter --------------------------------------------------
        keep, drop = filter_admin_entries(current)

        print(f"\n[*] Keeping: {len(keep)} admin entries")
        for e in keep:
            print(f"      KEEP  {e}")
        print(f"[*] Removing: {len(drop)} entries (binding-targets / stray fabric)")
        for e in drop:
            print(f"      DROP  {e}")

        if not keep:
            sys.exit("ERROR: no admin entry found. ABORT — otherwise you lose HA access!")

        if drop == []:
            print("\n[*] ACL: nothing to do — already clean.")
        elif args.dry_run:
            print("\n[*] --dry-run: nothing written to ACL.")
        else:
            # ----- 3) Write new ACL -----------------------------------
            print(f"\n[*] Writing new ACL ({len(keep)} entries) ...")
            resp = await send_command(ws, "write_attribute", {
                "node_id": args.node,
                "attribute_path": ACL_PATH,
                "value": keep,
            }, message_id="write-acl")

            if "error_code" in resp or "error" in resp:
                sys.exit(f"ERROR during write_attribute (acl): {resp}")

            print(f"[*] OK — response: {resp.get('result', resp)}")

        # ----- 4) Find endpoint(s) with a Binding cluster ---------------
        if args.endpoint is not None:
            binding_endpoints = [args.endpoint]
        else:
            print(f"\n[*] Auto-detecting Binding cluster endpoint(s) on node {args.node} ...")
            binding_endpoints = await discover_binding_endpoints(ws, args.node)

        if not binding_endpoints:
            print("[*] No Binding cluster found on this node — nothing to clean up.")
        else:
            print(f"[*] Binding cluster found on endpoint(s): {binding_endpoints}")

        # ----- 5) Read and clear the binding table on each endpoint -----
        for endpoint in binding_endpoints:
            binding_path = BINDING_PATH_TMPL.format(endpoint=endpoint)
            print(f"\n[*] Reading binding table of node {args.node} "
                  f"endpoint {endpoint} (path {binding_path}) ...")
            resp = await send_command(ws, "read_attribute", {
                "node_id": args.node,
                "attribute_path": binding_path,
            }, message_id=f"read-binding-{endpoint}")

            if "error_code" in resp or "error" in resp:
                sys.exit(f"ERROR during read_attribute (binding, endpoint {endpoint}): {resp}")

            current_bindings = _normalize_result(resp, binding_path)

            print(f"[*] Current binding table ({len(current_bindings)} entries):")
            for i, e in enumerate(current_bindings):
                print(f"      [{i}] {e}")

            if not current_bindings:
                print("[*] Binding table: nothing to do — already empty.")
                continue

            if args.dry_run:
                print("[*] --dry-run: nothing written to binding table.")
                continue

            print("[*] Clearing binding table (writing empty list) ...")
            resp = await send_command(ws, "write_attribute", {
                "node_id": args.node,
                "attribute_path": binding_path,
                "value": [],
            }, message_id=f"write-binding-{endpoint}")

            if "error_code" in resp or "error" in resp:
                sys.exit(f"ERROR during write_attribute (binding, endpoint {endpoint}): {resp}")

            print(f"[*] OK — response: {resp.get('result', resp)}")

        # ----- 6) Group Key Management: inspect capacity + fabric groups ----
        if args.skip_groups:
            print("\n[*] Group cleanup skipped (--skip-groups).")
        else:
            print(f"\n[*] Reading Group Key Management state on node {args.node} ...")
            resp = await send_command(ws, "read_attribute", {
                "node_id": args.node,
                "attribute_path": GROUP_KEY_MAP_PATH,
            }, message_id="read-groupkeymap")

            if "error_code" in resp or "error" in resp:
                sys.exit(f"ERROR during read_attribute (group key map): {resp}")

            group_key_map = _normalize_result(resp, GROUP_KEY_MAP_PATH)

            resp = await send_command(ws, "read_attribute", {
                "node_id": args.node,
                "attribute_path": MAX_GROUPS_PER_FABRIC_PATH,
            }, message_id="read-maxgroups")
            max_groups = _normalize_scalar(resp, MAX_GROUPS_PER_FABRIC_PATH)

            resp = await send_command(ws, "read_attribute", {
                "node_id": args.node,
                "attribute_path": MAX_GROUP_KEYS_PER_FABRIC_PATH,
            }, message_id="read-maxgroupkeys")
            max_group_keys = _normalize_scalar(resp, MAX_GROUP_KEYS_PER_FABRIC_PATH)

            print(f"[*] MaxGroupsPerFabric={max_groups!r} "
                  f"MaxGroupKeysPerFabric={max_group_keys!r}")
            print(f"[*] Current GroupKeyMap ({len(group_key_map)} entries, "
                  f"this fabric only):")
            for i, e in enumerate(group_key_map):
                print(f"      [{i}] {e}")

            # ----- 7) Clear device-local Groups cluster membership ---------
            print(f"\n[*] Auto-detecting Groups cluster endpoint(s) on node {args.node} ...")
            group_endpoints = await discover_group_endpoints(ws, args.node)

            if not group_endpoints:
                print("[*] No Groups cluster found on this node.")
            else:
                print(f"[*] Groups cluster found on endpoint(s): {group_endpoints}")

            for endpoint in group_endpoints:
                if args.dry_run:
                    print(f"[*] --dry-run: would send RemoveAllGroups to "
                          f"endpoint {endpoint}.")
                    continue

                print(f"[*] Sending RemoveAllGroups to endpoint {endpoint} ...")
                resp = await send_command(ws, "device_command", {
                    "node_id": args.node,
                    "endpoint_id": endpoint,
                    "cluster_id": GROUPS_CLUSTER_ID,
                    "command_name": "RemoveAllGroups",
                    "payload": {},
                }, message_id=f"remove-all-groups-{endpoint}")

                if "error_code" in resp or "error" in resp:
                    sys.exit(f"ERROR during device_command (RemoveAllGroups, "
                              f"endpoint {endpoint}): {resp}")

                print(f"[*] OK — response: {resp.get('result', resp)}")

            # ----- 8) Remove stale GroupKeySets (the actual scarce resource) --
            print(f"\n[*] Reading stored GroupKeySet indices on node {args.node} ...")
            stored_ids = await read_stored_keyset_ids(ws, args.node)
            print(f"[*] Stored GroupKeySet IDs: {stored_ids}")

            removable_ids = [i for i in stored_ids if i != 0]
            if 0 in stored_ids:
                print("[*] Keeping keyset ID 0 (IPK) — never remove this one.")

            if not removable_ids:
                print("[*] No removable GroupKeySets found — nothing to do here.")
            else:
                print(f"[*] Removable GroupKeySets: {removable_ids}")
                for keyset_id in removable_ids:
                    if args.dry_run:
                        print(f"[*] --dry-run: would remove GroupKeySet {keyset_id}.")
                        continue

                    print(f"[*] Removing GroupKeySet {keyset_id} ...")
                    resp = await send_command(ws, "device_command", {
                        "node_id": args.node,
                        "endpoint_id": 0,
                        "cluster_id": 63,
                        "command_name": "KeySetRemove",
                        "payload": {"groupKeySetID": keyset_id},
                    }, message_id=f"keyset-remove-{keyset_id}")

                    if "error_code" in resp or "error" in resp:
                        sys.exit(f"ERROR during device_command (KeySetRemove "
                                  f"{keyset_id}): {resp}")

                    print(f"[*] OK — response: {resp.get('result', resp)}")

            # ----- 9) Clear the fabric-scoped GroupKeyMap -------------------
            if not group_key_map:
                print("\n[*] GroupKeyMap: nothing to do — already empty.")
            elif args.dry_run:
                print("\n[*] --dry-run: nothing written to GroupKeyMap.")
            else:
                print("\n[*] Clearing GroupKeyMap (writing empty list) ...")
                resp = await send_command(ws, "write_attribute", {
                    "node_id": args.node,
                    "attribute_path": GROUP_KEY_MAP_PATH,
                    "value": [],
                }, message_id="write-groupkeymap")

                if "error_code" in resp or "error" in resp:
                    sys.exit(f"ERROR during write_attribute (group key map): {resp}")

                print(f"[*] OK — response: {resp.get('result', resp)}")

        print("\n[*] Done. Try adding the device to the group again.")
        return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()) or 0)
