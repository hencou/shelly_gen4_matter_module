#!/usr/bin/env python3
"""
=============================================================================
HOWTO: MATTER MULTICAST GROUP & BINDING SETUP (CUSTOM FIRMWARE)
=============================================================================

1. PREREQUISITES:
   Install 'websockets' on your machine if not already done:
   pip install websockets

2. USAGE:
   Run this script from your laptop. It installs a shared group encryption
   key on all nodes, writes the GroupKeyMap (required before AddGroup),
   adds lamps to the multicast group, and sets the binding table on the
   switch (Endpoint 1).

   python3 tools/create_matter_cluster_group.py --nodes 32 33 --switch 38 39 --group-id 0x0001

3. PARAMETERS:
   --nodes            List of Node IDs of the target lamps (e.g. 32 33)
   --switch           One or more Node IDs of the Shelly switches (e.g. 38 39)
   --group-id         The multicast Group ID (default: 0x0001)
   --switch-endpoint  The button endpoint on each switch (default: 1)
   --group-name       Name of the group (default: "Group")
   --server-ip        IP address of the HA Matter Server (default: 192.168.178.2)
   --server-port      Port of the HA Matter Server (default: 5580)

=============================================================================
"""

import argparse
import asyncio
import base64
import json
import sys
import websockets

DEFAULT_GROUP_ID   = 0x0001
DEFAULT_GROUP_NAME = "Group"

SERVER_IP   = "192.168.178.2"
SERVER_PORT = 5580

# KeySet ID used for group encryption. We use 1 (not 0/IPK) because some
# SDK versions cannot use the IPK directly for group messaging.
GROUP_KEYSET_ID = 1

# Default 128-bit epoch key (hex). All nodes in the group MUST share the
# same key to encrypt/decrypt multicast messages.  Change this if you want
# your own key; it must be exactly 32 hex characters (16 bytes).
# IMPORTANT: must match the key in app_main.cpp (kGroupEpochKey).
DEFAULT_EPOCH_KEY = "d0d1d2d3d4d5d6d7d8d9dadbdcdddedf"

class MatterRemoteClient:
    def __init__(self, ip, port):
        self.url = f"ws://{ip}:{port}/ws"
        self.ws = None
        self._message_id = 1

    async def connect(self):
        print(f"[*] Connecting to Matter Server at {self.url}...")
        self.ws = await websockets.connect(self.url)
        await self.ws.recv()

    async def send_command(self, command, args):
        msg_id = str(self._message_id)
        self._message_id += 1
        payload = {"message_id": msg_id, "command": command, "args": args}
        await self.ws.send(json.dumps(payload))
        async for message in self.ws:
            response = json.loads(message)
            if response.get("message_id") == msg_id:
                if "error_code" in response and response["error_code"] != 0:
                    raise Exception(f"{response.get('details', response)}")
                return response.get("result")

    async def close(self):
        if self.ws: await self.ws.close()

async def find_lamp_endpoint(client, node_id):
    """Find the endpoint with OnOff (6) + Groups (4) clusters on a lamp.

    Reads the Descriptor cluster (29) ServerList attribute (3) on each
    endpoint to find one that has both the OnOff and Groups server clusters.
    Returns the endpoint number, or None if not found.
    """
    try:
        # Read PartsList from root endpoint to discover all endpoints
        result = await client.send_command("read_attribute", {
            "node_id": node_id,
            "attribute_path": "0/29/3"  # Descriptor.PartsList
        })
        parts_list = result.get("0/29/3", [])
    except Exception:
        parts_list = []

    # Check each endpoint for OnOff (6) + Groups (4)
    for ep in parts_list:
        try:
            result = await client.send_command("read_attribute", {
                "node_id": node_id,
                "attribute_path": f"{ep}/29/1"  # Descriptor.ServerList
            })
            server_list = result.get(f"{ep}/29/1", [])
            if 6 in server_list and 4 in server_list:  # OnOff + Groups
                return ep
        except Exception:
            continue
    return None


async def run_logic(args):
    client = MatterRemoteClient(args.server_ip, args.server_port)
    await client.connect()

    print(f"\n" + "="*60)
    print(f"[*] START MATTER MULTICAST CONFIGURATION")
    print(f"[*] Group ID:    0x{args.group_id:04X} ({args.group_name})")
    print(f"[*] Target lamps: {args.nodes}")
    if len(args.switch) == 1:
        print(f"[*] Switch:      Node {args.switch[0]} (Endpoint {args.switch_endpoint})")
    else:
        print(f"[*] Switches:    {args.switch} (Endpoint {args.switch_endpoint})")
    print("="*60 + "\n")

    try:
        # =======================================================================
        # STEP 0: DISCOVER LAMP ENDPOINTS
        # =======================================================================
        # Different lamps may have the OnOff/Groups clusters on different
        # endpoints. Auto-detect unless --lamp-endpoint is given.
        lamp_endpoints = {}  # node_id -> endpoint
        if args.lamp_endpoint is not None:
            for node_id in args.nodes:
                lamp_endpoints[node_id] = args.lamp_endpoint
            print(f"[STEP 0] Using lamp endpoint {args.lamp_endpoint} (manual override)")
        else:
            print("[STEP 0] Discovering lamp endpoints (OnOff + Groups clusters)...")
            for node_id in args.nodes:
                ep = await find_lamp_endpoint(client, node_id)
                if ep is not None:
                    lamp_endpoints[node_id] = ep
                    print(f"  --> Lamp {node_id}: endpoint {ep}")
                else:
                    lamp_endpoints[node_id] = 1  # fallback
                    print(f"  --> Lamp {node_id}: not found, using default endpoint 1")
        print()
        # =======================================================================
        # STEP 1: INSTALL GROUP ENCRYPTION KEY ON ALL NODES (KeySetWrite)
        # =======================================================================
        # Matter group multicast messages are encrypted with a shared key.
        # All nodes (sender + receivers) must have the same KeySet installed.
        # We use KeySet ID 1 (not 0/IPK) because some SDK versions have issues
        # using the IPK directly for group encryption.
        print("[STEP 1] Installing group encryption key (KeySetWrite)...")

        # Convert hex key to base64 (python-matter-server API expects
        # bytes fields as base64-encoded strings)
        key_bytes = bytes.fromhex(args.epoch_key)
        if len(key_bytes) != 16:
            print(f"[!] ERROR: epoch key must be exactly 16 bytes (32 hex chars)")
            return
        epoch_key_b64 = base64.b64encode(key_bytes).decode()
        print(f"    Key (hex):    {args.epoch_key}")
        print(f"    Key (base64): {epoch_key_b64}")

        all_nodes = list(args.nodes) + list(args.switch)
        for node_id in all_nodes:
            label = f"Switch (Node {node_id})" if node_id in args.switch else f"Lamp (Node {node_id})"
            print(f"  --> KeySetWrite on {label}...")
            try:
                await client.send_command("device_command", {
                    "node_id": node_id,
                    "endpoint_id": 0,
                    "cluster_id": 63,  # GroupKeyManagement
                    "command_name": "KeySetWrite",
                    "payload": {
                        "groupKeySet": {
                            "groupKeySetID": GROUP_KEYSET_ID,
                            "groupKeySecurityPolicy": 0,  # TrustFirst
                            "epochKey0": epoch_key_b64,
                            "epochStartTime0": 1,  # 1 microsecond = always valid
                            "epochKey1": None,
                            "epochStartTime1": None,
                            "epochKey2": None,
                            "epochStartTime2": None,
                        }
                    }
                })
                print(f"    [OK] KeySet {GROUP_KEYSET_ID} installed")
            except Exception as e:
                print(f"    [!] KeySetWrite failed: {e}")

        # =======================================================================
        # STEP 2: WRITE GROUPKEYMAP ON ALL NODES
        # =======================================================================
        # Maps the group ID to our KeySet so the SDK can find the encryption
        # key.  MUST be done BEFORE AddGroup — the Matter spec requires a
        # GroupKeyMap entry to exist before a node can join a group.
        print("\n" + "-"*50)
        print(f"[STEP 2] Writing GroupKeyMap (group -> keyset {GROUP_KEYSET_ID})...")

        # GroupKeyMapStruct uses integer TLV tag keys — string field names
        # ("groupId"/"groupKeySetID") get silently zeroed by the device, so
        # the write appears to succeed but the readback stays empty (exactly
        # like the ACL and binding structs). "1" = groupId, "2" = groupKeySetID
        # ("254" = fabricIndex, auto-filled by the server).
        group_key_entry = {
            "1": args.group_id,
            "2": GROUP_KEYSET_ID,
        }

        for node_id in all_nodes:
            label = f"Switch (Node {node_id})" if node_id in args.switch else f"Lamp (Node {node_id})"
            print(f"  --> GroupKeyMap on {label}...")
            try:
                await client.send_command("write_attribute", {
                    "node_id": node_id,
                    "attribute_path": "0/63/0",
                    "value": [group_key_entry]
                })
                print(f"    [OK] GroupKeyMap written")
            except Exception as e:
                print(f"    [!] GroupKeyMap failed: {e}")

        # Verify GroupKeyMap on lamps
        print("\n    Verifying GroupKeyMap on lamps...")
        for node_id in args.nodes:
            try:
                result = await client.send_command("read_attribute", {
                    "node_id": node_id,
                    "attribute_path": "0/63/0"
                })
                print(f"    Lamp {node_id} GroupKeyMap: {json.dumps(result, indent=6)}")
            except Exception as e:
                print(f"    [!] Lamp {node_id} GroupKeyMap read failed: {e}")

        # Verify KeySet on lamps
        print("\n    Verifying KeySet on lamps...")
        for node_id in args.nodes:
            try:
                result = await client.send_command("device_command", {
                    "node_id": node_id,
                    "endpoint_id": 0,
                    "cluster_id": 63,
                    "command_name": "KeySetRead",
                    "payload": {"groupKeySetID": GROUP_KEYSET_ID}
                })
                print(f"    Lamp {node_id} KeySet {GROUP_KEYSET_ID}: {result}")
            except Exception as e:
                print(f"    [!] Lamp {node_id} KeySet read failed: {e}")

        # =======================================================================
        # STEP 3: ADD LAMPS TO THE MULTICAST GROUP
        # =======================================================================
        # Now that GroupKeyMap is in place, AddGroup can succeed.
        print("\n" + "-"*50)
        print("[STEP 3] Adding lamps to multicast group...")
        for node_id in args.nodes:
            print(f"  --> Configure Lamp (Node {node_id})...")
            ep = lamp_endpoints[node_id]
            print(f"    (endpoint {ep})")
            try:
                result = await client.send_command("device_command", {
                    "node_id": node_id, "endpoint_id": ep, "cluster_id": 4,
                    "command_name": "AddGroup",
                    "payload": {"groupID": args.group_id, "groupName": args.group_name}
                })
                status = result.get("status", "?") if isinstance(result, dict) else "?"
                print(f"    AddGroup response: {result}")
                if status == 0:
                    print(f"    [OK] AddGroup successful")
                else:
                    print(f"    [!] AddGroup returned status {status}")
            except Exception as e:
                print(f"    [!] AddGroup failed: {e}")

            # Verify: read group table back
            try:
                result = await client.send_command("device_command", {
                    "node_id": node_id, "endpoint_id": ep, "cluster_id": 4,
                    "command_name": "ViewGroup",
                    "payload": {"groupID": args.group_id}
                })
                vg_status = result.get("status", "?") if isinstance(result, dict) else "?"
                if vg_status == 0:
                    print(f"    [OK] ViewGroup confirmed: {result}")
                else:
                    print(f"    [!] ViewGroup NOT_FOUND (status={vg_status}): {result}")
            except Exception as e:
                print(f"    [!] ViewGroup verify failed: {e}")

        # =======================================================================
        # STEP 4: ADD GROUP ACL ON LAMPS
        # =======================================================================
        # Matter Access Control requires an explicit ACL entry that allows
        # group commands.  Without this, the lamp decrypts the message but
        # silently rejects it because no ACL grants Operate privilege for
        # the group's authMode.
        #
        # IMPORTANT: python-matter-server uses integer TLV tag keys for
        # AccessControlEntryStruct fields:
        #   "1" = privilege, "2" = authMode, "3" = subjects,
        #   "4" = targets,  "254" = fabricIndex
        # Both read_attribute and write_attribute use this format.
        # Do NOT use string field names ("privilege", "authMode", etc.)
        # — they get silently zeroed out by the device.
        print("\n" + "-"*50)
        print("[STEP 4] Adding Group ACL entry on lamps...")

        # TLV tag names for logging
        ACL_FIELD = {"1": "privilege", "2": "authMode", "3": "subjects",
                     "4": "targets", "254": "fabricIndex"}

        def acl_get(entry, field_name):
            """Get ACL field by name, looking up the TLV tag."""
            tag = {v: k for k, v in ACL_FIELD.items()}.get(field_name)
            if tag and tag in entry:
                return entry[tag]
            # Fallback: try string key directly (some server versions)
            return entry.get(field_name)

        for node_id in args.nodes:
            print(f"  --> ACL on Lamp (Node {node_id})...")
            try:
                # Read current ACL (cluster 31, attribute 0, endpoint 0)
                acl = await client.send_command("read_attribute", {
                    "node_id": node_id,
                    "attribute_path": "0/31/0"
                })
                # Extract the list (response format: {"0/31/0": [...]} )
                acl_key = list(acl.keys())[0] if isinstance(acl, dict) else None
                acl_list = acl[acl_key] if acl_key else acl
                if not isinstance(acl_list, list):
                    acl_list = [acl_list] if acl_list else []

                # Dump raw ACL data for debugging
                print(f"    Raw ACL data: {json.dumps(acl_list, indent=2)}")
                print(f"    Current ACL ({len(acl_list)} entries):")
                for i, e in enumerate(acl_list):
                    priv = acl_get(e, "privilege")
                    auth = acl_get(e, "authMode")
                    subj = acl_get(e, "subjects")
                    print(f"      #{i+1}: privilege={priv} authMode={auth} subjects={subj}")

                # Check if a group ACL entry (authMode=3) already exists
                group_acl_exists = False
                for entry in acl_list:
                    auth = acl_get(entry, "authMode")
                    subj = acl_get(entry, "subjects") or []
                    if auth == 3 and args.group_id in subj:
                        group_acl_exists = True
                        break

                if group_acl_exists:
                    print(f"    [OK] Group ACL already present")
                else:
                    # Keep existing entries in their ORIGINAL format (integer
                    # TLV keys) — do NOT convert to string field names.
                    # Only strip fabricIndex ("254") — the server auto-fills it.
                    clean = []
                    for entry in acl_list:
                        e = {k: v for k, v in entry.items() if str(k) != "254"}
                        # Skip broken entries (privilege=0 or missing)
                        priv = acl_get({"x": None, **entry}, "privilege")
                        if priv is not None and priv > 0:
                            clean.append(e)
                        else:
                            print(f"    [!] Dropping broken entry: {e}")

                    # New group ACL entry — use integer TLV tag keys
                    group_acl = {
                        "1": 3,                    # privilege = Operate
                        "2": 3,                    # authMode = Group
                        "3": [args.group_id],      # subjects = [group_id]
                        "4": None,                 # targets = all
                    }

                    # Admin entries (privilege=5) must come first
                    admin = [e for e in clean if acl_get(e, "privilege") == 5]
                    other = [e for e in clean if acl_get(e, "privilege") != 5]
                    new_acl = admin + other + [group_acl]

                    print(f"    Writing ACL ({len(new_acl)} entries):")
                    for i, e in enumerate(new_acl):
                        print(f"      #{i+1}: {json.dumps(e)}")

                    await client.send_command("write_attribute", {
                        "node_id": node_id,
                        "attribute_path": "0/31/0",
                        "value": new_acl
                    })

                    # Verify: read back and check
                    verify = await client.send_command("read_attribute", {
                        "node_id": node_id,
                        "attribute_path": "0/31/0"
                    })
                    vkey = list(verify.keys())[0] if isinstance(verify, dict) else None
                    vlist = verify[vkey] if vkey else verify
                    if not isinstance(vlist, list):
                        vlist = [vlist] if vlist else []
                    print(f"    Verify ACL ({len(vlist)} entries):")
                    for i, e in enumerate(vlist):
                        print(f"      #{i+1}: {json.dumps(e)}")
                    # Check group entry is present and correct
                    found = False
                    for e in vlist:
                        if acl_get(e, "authMode") == 3:
                            p = acl_get(e, "privilege")
                            if p == 3:
                                found = True
                            else:
                                print(f"    [!] WARNING: Group entry has privilege={p}, expected 3")
                    if found:
                        print(f"    [OK] Group ACL verified (Operate privilege for group 0x{args.group_id:04X})")
                    else:
                        print(f"    [!] WARNING: Group ACL NOT found after write!")

            except Exception as e:
                print(f"    [!] ACL setup failed: {e}")

        # =======================================================================
        # STEP 5: WRITE BINDING ON EACH SWITCH
        # =======================================================================
        print("\n" + "-"*50)
        print(f"[STEP 5] Writing binding table to {len(args.switch)} switch(es)...")

        # Binding TargetStruct (cluster 0x1E / 30, attribute 0) uses integer TLV
        # field IDs — string keys ("group"/"cluster") get silently zeroed by the
        # device, exactly like the ACL struct in STEP 4. Group binding = group (2)
        # + cluster (4) only, no node/endpoint. fabricIndex (254) is auto-filled.
        bindings_tlv = [
            {"2": args.group_id, "4": 6},    # OnOff
            {"2": args.group_id, "4": 8},    # LevelControl (dimming)
            {"2": args.group_id, "4": 768},  # ColorControl (0x0300)
        ]

        for sw in args.switch:
            print(f"  --> Binding on Switch (Node {sw}, Endpoint {args.switch_endpoint})...")
            # write_attribute with integer TLV keys is the reliable path.
            # set_node_binding returns success but sends no WriteRequest for
            # group bindings, so the write never reaches the device.
            try:
                result = await client.send_command("write_attribute", {
                    "node_id": sw,
                    "attribute_path": f"{args.switch_endpoint}/30/0",
                    "value": bindings_tlv
                })
                print(f"    [OK] Binding written via write_attribute (group TLV)!")
                print(f"    Result: {result}")
            except Exception as e:
                print(f"    [!] write_attribute failed ({e}), trying set_node_binding...")
                try:
                    result = await client.send_command("set_node_binding", {
                        "node_id": sw,
                        "endpoint": args.switch_endpoint,
                        "bindings": [
                            {"group": args.group_id, "cluster": 6},
                            {"group": args.group_id, "cluster": 8},
                            {"group": args.group_id, "cluster": 768},
                        ]
                    })
                    print(f"    [OK] Binding written via set_node_binding! Result: {result}")
                except Exception as e2:
                    print(f"    [!] set_node_binding also failed: {e2}")

        # =======================================================================
        # STEP 6: VERIFICATION - READ BINDING BACK FROM EACH SWITCH
        # =======================================================================
        print("\n" + "-"*50)
        print(f"[STEP 6] Verification - reading binding back from {len(args.switch)} switch(es)...")
        for sw in args.switch:
            print(f"  --> Binding on Switch (Node {sw})...")
            try:
                result = await client.send_command("read_attribute", {
                    "node_id": sw,
                    "attribute_path": f"{args.switch_endpoint}/30/0"
                })
                print(f"    Current binding table: {json.dumps(result, indent=2)}")
            except Exception as e:
                print(f"    [!] Could not read binding: {e}")

        print("\n" + "="*60)
        print("[*] SCRIPT COMPLETE!")
        print("="*60)
        sw_word = "switches" if len(args.switch) > 1 else "switch"
        print(f"\nTest the {sw_word} now — press the button briefly and check")
        print("whether all lamps in the group toggle simultaneously.")
        print("\nIf it does not work:")
        print("  1. Check the Shelly log for 'send_onoff_multicast' lines")
        print("  2. Look for OK (sent) vs FAILED (SDK error)")
        print(f"  3. Do NOT restart the Shelly — bindings should work immediately")

    except Exception as general_error:
        print(f"\n[!] General error: {general_error}")
    finally:
        await client.close()

if __name__ == "__main__":
    ap = argparse.ArgumentParser(
        description="Configure Matter multicast group + binding for Shelly switch(es)")
    ap.add_argument("--nodes", type=int, nargs="+", required=True,
                    help="Node IDs of the target lamps")
    ap.add_argument("--switch", type=int, nargs="+", required=True,
                    help="Node ID(s) of the Shelly switch(es)")
    ap.add_argument("--group-id", type=lambda x: int(x, 0), default=DEFAULT_GROUP_ID,
                    help="Multicast Group ID (default: 0x0001)")
    ap.add_argument("--group-name", default=DEFAULT_GROUP_NAME,
                    help="Name of the group (default: Group)")
    ap.add_argument("--switch-endpoint", type=int, default=1,
                    help="Endpoint on the switch (default: 1 = pushbutton/toggle)")
    ap.add_argument("--lamp-endpoint", type=int, default=None,
                    help="Override endpoint on lamps (default: auto-detect)")
    ap.add_argument("--server-ip", default=SERVER_IP,
                    help=f"IP of the HA Matter Server (default: {SERVER_IP})")
    ap.add_argument("--server-port", type=int, default=SERVER_PORT,
                    help=f"Port of the HA Matter Server (default: {SERVER_PORT})")
    ap.add_argument("--epoch-key", default=DEFAULT_EPOCH_KEY,
                    help="128-bit hex key for group encryption (32 hex chars)")
    asyncio.run(run_logic(ap.parse_args()))
