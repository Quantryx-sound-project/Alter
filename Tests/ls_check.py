#!/usr/bin/env python3
"""
ls_check.py — overenie Lemon Squeezy PRED tym, nez zbuildis Alter.

Potrebujes len Python 3 (ziadne pip balicky, len stdlib) a jeden licencny
kluc zo svojho obchodu — pokojne z test modu.

POUZITIE
--------
  # zisti store_id / product_id / variant_id a vygeneruj riadok do configu
  python ls_check.py activate  <LICENCNY-KLUC>

  # over kluc (bez zaberania slotu)
  python ls_check.py validate  <LICENCNY-KLUC> [INSTANCE-ID]

  # uvolni slot, ktory zabral activate
  python ls_check.py deactivate <LICENCNY-KLUC> <INSTANCE-ID>

  # cely kolobeh naraz: activate -> validate -> deactivate
  python ls_check.py roundtrip <LICENCNY-KLUC>

  # skontroluj, ci AlterLicenseConfig.h sedi s realitou
  python ls_check.py checkconfig <LICENCNY-KLUC>

TIP
---
Spusti "activate" pre KAZDY variant, ktory predavas. Skript ti vypise
hotove riadky do kVariantMap v ../AlterLicenseConfig.h.

POZOR: kazdy "activate" zabera aktivacny slot. Skript ti vzdy pripomenie,
ako ho vratit; "roundtrip" ho vrati sam.
"""

import json
import re
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

API = "https://api.lemonsqueezy.com/v1/licenses"
CONFIG = Path(__file__).resolve().parent.parent / "AlterLicenseConfig.h"

# ---------------------------------------------------------------- vzhlad ----
class C:
    OK = "\033[92m"; WARN = "\033[93m"; ERR = "\033[91m"
    DIM = "\033[2m"; B = "\033[1m"; CYAN = "\033[96m"; END = "\033[0m"

def head(t):  print(f"\n{C.CYAN}{C.B}=== {t} ==={C.END}")
def ok(t):    print(f"  {C.OK}OK{C.END}    {t}")
def warn(t):  print(f"  {C.WARN}POZOR{C.END} {t}")
def bad(t):   print(f"  {C.ERR}CHYBA{C.END} {t}")
def dim(t):   print(f"  {C.DIM}{t}{C.END}")


def call(endpoint, **fields):
    """POST na License API. Vracia (json, http_status)."""
    data = urllib.parse.urlencode(fields).encode()
    req = urllib.request.Request(
        f"{API}/{endpoint}",
        data=data,
        headers={
            "Accept": "application/json",
            "Content-Type": "application/x-www-form-urlencoded",
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            return json.loads(r.read().decode()), r.status
    except urllib.error.HTTPError as e:
        body = e.read().decode()
        try:
            return json.loads(body), e.code
        except json.JSONDecodeError:
            return {"error": body or str(e)}, e.code
    except urllib.error.URLError as e:
        bad(f"Sietova chyba: {e.reason}")
        sys.exit(1)


def show(resp):
    """Vypise podstatne z odpovede a vrati meta dict."""
    lk = resp.get("license_key") or {}
    meta = resp.get("meta") or {}
    inst = resp.get("instance") or {}

    if lk:
        head("Licencny kluc")
        print(f"  status            {lk.get('status')}")
        print(f"  aktivacie         {lk.get('activation_usage')} / {lk.get('activation_limit')}")
        print(f"  plati do          {lk.get('expires_at') or 'neobmedzene (lifetime)'}")

    if inst:
        head("Instancia")
        print(f"  instance_id       {inst.get('id')}")
        print(f"  nazov             {inst.get('name')}")

    if meta:
        head("Identita produktu  (toto ide do AlterLicenseConfig.h)")
        print(f"  {C.B}store_id          {meta.get('store_id')}{C.END}")
        print(f"  {C.B}product_id        {meta.get('product_id')}{C.END}")
        print(f"  {C.B}variant_id        {meta.get('variant_id')}{C.END}")
        print(f"  product_name      {meta.get('product_name')}")
        print(f"  variant_name      {meta.get('variant_name')}")
        print(f"  customer_email    {meta.get('customer_email')}")

    return meta


def suggest(meta):
    if not meta:
        return

    vn = (meta.get("variant_name") or "").lower()
    pn = (meta.get("product_name") or "").lower()
    both = vn + " " + pn

    if "pro" in both:
        tier = "Pro"
    elif "creator" in both:
        tier = "Creator"
    elif "listener" in both:
        tier = "Listener"
    else:
        tier = "???"

    head("Skopiruj do Shared/AlterLicenseConfig.h")
    print(f"  inline constexpr std::int64_t kStoreId   = {meta.get('store_id')};")
    print(f"  inline constexpr std::int64_t kProductId = {meta.get('product_id')};")
    print()
    label = f"{meta.get('product_name')} — {meta.get('variant_name')}"
    print(f"  {{ {meta.get('variant_id')}, TierId::{tier}, \"{label}\" }},")

    if tier == "???":
        warn("Tier som z nazvu neuhadol — dopln ho rucne "
             "(Listener / Creator / Pro).")


# ------------------------------------------------------------- prikazy ------

def cmd_activate(key, quiet=False):
    resp, _ = call("activate", license_key=key, instance_name="ls_check.py")

    if not resp.get("activated"):
        bad(f"Aktivacia zlyhala: {resp.get('error')}")
        show(resp)
        return None

    ok("Aktivovane")
    meta = show(resp)

    if not quiet:
        suggest(meta)
        inst = (resp.get("instance") or {}).get("id")
        warn("Tato aktivacia zabrala SLOT. Uvolni ho:")
        dim(f"python ls_check.py deactivate {key} {inst}")

    return resp


def cmd_validate(key, instance_id=None):
    fields = {"license_key": key}
    if instance_id:
        fields["instance_id"] = instance_id

    resp, _ = call("validate", **fields)

    if resp.get("valid"):
        ok("Kluc je platny")
    else:
        bad(f"Kluc NIE je platny: {resp.get('error')}")

    show(resp)
    return resp


def cmd_deactivate(key, instance_id):
    resp, _ = call("deactivate", license_key=key, instance_id=instance_id)

    if resp.get("deactivated"):
        ok("Slot uvolneny")
    else:
        bad(f"Deaktivacia zlyhala: {resp.get('error')}")

    show(resp)
    return resp


def cmd_roundtrip(key):
    head("1/3  ACTIVATE")
    a = cmd_activate(key, quiet=True)
    if not a:
        return

    inst = (a.get("instance") or {}).get("id")

    head("2/3  VALIDATE (s instance_id)")
    cmd_validate(key, inst)

    head("3/3  DEACTIVATE")
    cmd_deactivate(key, inst)

    suggest(a.get("meta") or {})
    head("Hotovo")
    ok("Cely kolobeh presiel. Alter sa na Lemon Squeezy vie napojit.")


def cmd_checkconfig(key):
    if not CONFIG.exists():
        bad(f"Nenasiel som {CONFIG}")
        return

    src = CONFIG.read_text(encoding="utf-8")

    def const(name):
        m = re.search(rf"kStoreId\s*=\s*(-?\d+)" if name == "kStoreId"
                      else rf"{name}\s*=\s*(-?\d+)", src)
        return int(m.group(1)) if m else None

    cfg_store = const("kStoreId")
    cfg_prod = const("kProductId")
    variants = {int(v): t for v, t in
                re.findall(r"\{\s*(\d+)\s*,\s*TierId::(\w+)", src)}

    head("Overujem kluc voci Lemon Squeezy")
    resp, _ = call("validate", license_key=key)

    if not resp.get("valid"):
        bad(f"Kluc nie je platny: {resp.get('error')}")
        return

    meta = resp.get("meta") or {}
    ok("Kluc je platny")

    head("Porovnanie s AlterLicenseConfig.h")
    problems = 0

    if cfg_store in (None, 0):
        bad("kStoreId nie je vyplnene -> Alter odomkne HOCIJAKY kluc "
            "z celeho Lemon Squeezy")
        dim(f"nastav: kStoreId = {meta.get('store_id')};")
        problems += 1
    elif cfg_store != meta.get("store_id"):
        bad(f"kStoreId = {cfg_store}, ale kluc je z obchodu {meta.get('store_id')}")
        problems += 1
    else:
        ok(f"kStoreId sedi ({cfg_store})")

    if cfg_prod in (None, 0):
        warn("kProductId nie je vyplnene (volitelne, ale odporucane)")
        dim(f"nastav: kProductId = {meta.get('product_id')};")
    elif cfg_prod != meta.get("product_id"):
        bad(f"kProductId = {cfg_prod}, ale kluc je z produktu {meta.get('product_id')}")
        problems += 1
    else:
        ok(f"kProductId sedi ({cfg_prod})")

    vid = meta.get("variant_id")
    if vid in variants:
        ok(f"variant {vid} -> {variants[vid]}")
    else:
        bad(f"variant {vid} nie je v kVariantMap -> tento kluc by skoncil ako Demo")
        suggest(meta)
        problems += 1

    stale = [v for v in variants if v == 0]
    if stale:
        warn(f"{len(stale)} riadkov v kVariantMap ma este variantId = 0")

    print()
    if problems == 0:
        ok("Konfiguracia je pripravena.")
    else:
        bad(f"{problems} problem(ov) na doriesenie.")


# ---------------------------------------------------------------- main ------

def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)

    cmd, key = sys.argv[1], sys.argv[2]
    rest = sys.argv[3:]

    if cmd == "activate":
        cmd_activate(key)
    elif cmd == "validate":
        cmd_validate(key, rest[0] if rest else None)
    elif cmd == "deactivate":
        if not rest:
            bad("Chyba INSTANCE-ID.")
            sys.exit(1)
        cmd_deactivate(key, rest[0])
    elif cmd == "roundtrip":
        cmd_roundtrip(key)
    elif cmd == "checkconfig":
        cmd_checkconfig(key)
    else:
        print(__doc__)
        sys.exit(1)


if __name__ == "__main__":
    main()
