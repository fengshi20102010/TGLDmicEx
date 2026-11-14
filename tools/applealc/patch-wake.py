#!/usr/bin/env python3
"""Add a wake-time HDA verb to one AppleALC layout. Data-only patch, no rebuild needed.

AppleALC keeps the codec pin configs in AppleALC.kext/Contents/Info.plist (HDAConfigDefault). With
WakeVerbReinit = true it replays WakeConfigData whenever the audio driver comes back from its sleep
power state (see AppleALC/kern_alc.cpp, AlcEnabler::performPowerChange). Layout 11 (ALC257, Lenovo T480
by MacPeet) only replays "nid 0x14 SET_EAPD 0x02" (the speaker amp). On a Lenovo XiaoXin Pro 13 2020 the
headphone jack then came back with low volume and crackling after sleep until the input was switched or
the plug was re-seated. Layouts 86 / 96 / 97 of the same codec already replay
"nid 0x19 SET_PIN_WIDGET_CONTROL 0x25"; this script adds that verb to layout 11.

usage:
  patch-wake.py /Volumes/EFI/EFI/OC/Kexts/AppleALC.kext             apply (nid 0x19, value 0x25)
  patch-wake.py <kext or Info.plist> --check                         exit 0 applied, 3 not applied
  patch-wake.py <kext or Info.plist> --value 0x24                    replace the value of that verb
  patch-wake.py <kext or Info.plist> --revert                        remove the verb again
  patch-wake.py <kext or Info.plist> --dry-run                       show what would change, write nothing

Only the one WakeConfigData value is touched; every other value of the plist is compared before writing and
the script aborts if anything else would differ. The original is copied to --backup-dir first.
The code signature of the kext is not refreshed (OpenCore injects kexts without checking it).
Exit codes: 0 ok / already in the requested state, 1 error, 3 --check and not applied.
"""
import argparse
import copy
import os
import plistlib
import shutil
import sys
import tempfile
import time

SET_PIN_WIDGET_CONTROL = 0x707


def die(msg, code=1):
    print("error: " + msg, file=sys.stderr)
    sys.exit(code)


def plist_path(p):
    p = os.path.abspath(os.path.expanduser(p))
    if os.path.isdir(p):
        p = os.path.join(p, "Contents", "Info.plist")
    if not os.path.isfile(p):
        die("not found: " + p)
    return p


def entries(plist, codec, layout):
    out = []
    for pers in plist.get("IOKitPersonalities", {}).values():
        for c in pers.get("HDAConfigDefault", []) or []:
            if c.get("CodecID") == codec and c.get("LayoutID") == layout and c.get("FuncGroup", 1) == 1:
                out.append(c)
    return out


def words(data):
    if len(data) % 4:
        die("WakeConfigData length %d is not a multiple of 4" % len(data))
    return [data[i:i + 4] for i in range(0, len(data), 4)]


def verb_word(nid, value):
    return ((nid << 20) | (SET_PIN_WIDGET_CONTROL << 8) | value).to_bytes(4, "big")


def same_target(w, nid):
    """Same node and same verb (SET_PIN_WIDGET_CONTROL), whatever the value."""
    x = int.from_bytes(w, "big")
    return ((x >> 20) & 0xFF) == nid and ((x >> 8) & 0xFFF) == SET_PIN_WIDGET_CONTROL


def leaves(o, path=""):
    if isinstance(o, dict):
        for k, v in o.items():
            yield from leaves(v, path + "/" + str(k))
    elif isinstance(o, list):
        for i, v in enumerate(o):
            yield from leaves(v, path + "[%d]" % i)
    else:
        yield path, o


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("target", help="AppleALC.kext or its Contents/Info.plist")
    ap.add_argument("--codec", type=lambda s: int(s, 0), default=0x10EC0257, help="codec id (default 0x10ec0257, ALC257)")
    ap.add_argument("--layout", type=int, default=11, help="layout id (default 11)")
    ap.add_argument("--nid", type=lambda s: int(s, 0), default=0x19, help="pin node (default 0x19, headset jack)")
    ap.add_argument("--value", type=lambda s: int(s, 0), default=0x25, help="pin widget control value (default 0x25)")
    ap.add_argument("--revert", action="store_true", help="remove the verb")
    ap.add_argument("--check", action="store_true", help="only report; exit 3 when not in the requested state")
    ap.add_argument("--dry-run", action="store_true", help="show the change, do not write")
    ap.add_argument("--backup-dir", default=".", help="where the original Info.plist is copied (default: current dir)")
    a = ap.parse_args()
    if not (0 <= a.nid <= 0xFF and 0 <= a.value <= 0xFF):
        die("--nid and --value must fit one byte")

    path = plist_path(a.target)
    raw = open(path, "rb").read()
    fmt = plistlib.FMT_XML if raw.lstrip().startswith(b"<?xml") else plistlib.FMT_BINARY
    old = plistlib.loads(raw)
    hits = entries(old, a.codec, a.layout)
    if len(hits) != 1:
        die("expected exactly one HDAConfigDefault entry for codec 0x%x layout %d, found %d (is this AppleALC.kext?)"
            % (a.codec, a.layout, len(hits)))
    entry = hits[0]
    if "WakeConfigData" not in entry or entry.get("WakeVerbReinit") is not True:
        die("layout %d has no WakeConfigData / WakeVerbReinit=true; refusing to invent one" % a.layout)

    ws = words(entry["WakeConfigData"])
    want = verb_word(a.nid, a.value)
    idx = [i for i, w in enumerate(ws) if same_target(w, a.nid)]
    desc = "nid 0x%02x SET_PIN_WIDGET_CONTROL 0x%02x (%s)" % (a.nid, a.value, want.hex())
    current = ws[idx[0]].hex() if idx else None
    print("%s: codec 0x%x layout %d, WakeConfigData = %s" % (path, a.codec, a.layout, entry["WakeConfigData"].hex()))

    if a.revert:
        done = not idx
        new_ws = [w for i, w in enumerate(ws) if i not in idx]
        action = "remove the verb for nid 0x%02x" % a.nid
    else:
        done = len(idx) >= 1 and all(ws[i] == want for i in idx)
        if idx:
            # replace the first occurrence, drop any duplicates of the same node/verb
            new_ws, seen = [], False
            for i, w in enumerate(ws):
                if i in idx:
                    if seen:
                        continue
                    seen = True
                    w = want
                new_ws.append(w)
            action = "replace %s by %s" % (current, want.hex())
        else:
            new_ws = ws + [want]
            action = "append " + desc
    state = "already in the requested state" if done else "needs change: " + action
    print("  " + state)

    if a.check:
        sys.exit(0 if done else 3)
    if done:
        sys.exit(0)

    new = copy.deepcopy(old)
    target = entries(new, a.codec, a.layout)[0]
    target["WakeConfigData"] = b"".join(new_ws)
    print("  new WakeConfigData = %s" % target["WakeConfigData"].hex())

    # nothing but that one value may differ
    before, after = dict(leaves(old)), dict(leaves(new))
    changed = [k for k in set(before) | set(after) if before.get(k) != after.get(k)]
    if len(changed) != 1 or not changed[0].endswith("/WakeConfigData"):
        die("self-check failed, would change %d values: %s" % (len(changed), changed[:3]))
    if a.dry_run:
        print("  dry run: nothing written")
        return

    out = plistlib.dumps(new, fmt=fmt, sort_keys=False)
    if dict(leaves(plistlib.loads(out))) != after:
        die("self-check failed: serialised plist does not read back identically")
    os.makedirs(a.backup_dir, exist_ok=True)
    bak = os.path.join(a.backup_dir, "AppleALC.Info.plist.orig-" + time.strftime("%Y%m%d-%H%M%S"))
    shutil.copy2(path, bak)
    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(path), prefix=".Info.plist.")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(out)
        shutil.copymode(path, tmp)
        os.replace(tmp, path)
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)
    print("  written. original saved to " + bak)
    print("  reboot to load it (OpenCore injects the kext from the EFI partition).")


if __name__ == "__main__":
    main()
