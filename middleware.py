#!/usr/bin/env python3
"""
vitastremio middleware
----------------------
Sits between a Stremio server (Docker, :11470) and a PS Vita client.

Responsibilities:
  * Talk HTTPS to Stremio addons so the Vita doesn't have to.
  * Flatten fat JSON into a line-based format trivial to parse in C.
  * Downscale posters to 128x186 JPEG.
  * Transcode via VAAPI into raw Annex-B H.264 + raw PCM.

Endpoints (all plain HTTP, no TLS):
  GET /catalog?type=movie&genre=            -> line format, one item per line
  GET /meta?type=movie&id=tt0111161         -> metadata + episode list
  GET /streams?type=movie&id=tt0111161      -> selectable stream list
  GET /poster?u=<urlencoded>                -> 128x186 JPEG
  GET /v?s=<streamkey>&t=<seconds>          -> raw H.264 Annex-B
  GET /a?s=<streamkey>&t=<seconds>          -> raw PCM s16le 32kHz stereo

Line format: fields separated by 0x1F (unit separator), records by 0x0A.
Chosen because it needs no parser on the Vita -- strtok on two delimiters.
"""

import json
import re
import os
import re
import shlex
import signal
import subprocess
import sys
import threading
import time
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# ---------------------------------------------------------------- config

STREMIO = os.environ.get("STREMIO_URL", "http://127.0.0.1:11470")
LISTEN_PORT = int(os.environ.get("VITA_PORT", "8480"))
RENDER_NODE = os.environ.get("VAAPI_DEVICE", "/dev/dri/renderD128")

# Vita panel is 960x544. Never send more pixels than that.
VID_W, VID_H = 960, 544
VID_BITRATE = "2500k"
VID_MAXRATE = "3000k"

# 32kHz stereo s16le = 1.024 Mbit/s. Uncompressed, but it buys us a
# perfect audio clock and one less decoder on the Vita. Combined with
# 2.5Mbit video this sits around 3.5Mbit -- within single-stream 2.4GHz
# 802.11n on a good link, tight on a bad one. Drop to mono if you stall.
AUD_RATE = 32000
AUD_CH = 2

POSTER_W, POSTER_H = 128, 186

# One Vita needs one video and one audio process. The cap stops repeated
# seeks from stacking orphaned encoders faster than they get reaped.
MAX_TRANSCODES = int(os.environ.get("MAX_TRANSCODES", "6"))
TRANSCODE_SLOTS = threading.BoundedSemaphore(MAX_TRANSCODES)

# Whitelist: ctype is interpolated into an addon URL path.
VALID_TYPES = {"movie", "series", "channel", "tv"}

# Where the Stremio auth key is cached, at mode 600. Only the key is ever
# written -- there is no password login, so there is no password to store.
AUTH_PATH = os.path.expanduser(
    os.environ.get("VITA_AUTH", "~/.vitastremio_auth"))

STREMIO_API = "https://api.strem.io/api"

FALLBACK_ADDONS = ["https://v3-cinemeta.strem.io/manifest.json"]

# Addons added through the web config page live here, so they survive a
# restart without anyone editing the source or exporting a variable.
ADDONS_PATH = os.path.expanduser(
    os.environ.get("VITA_ADDONS_FILE", "~/.vitastremio_addons.json"))


def normalize_manifest(url):
    """Accept anything a user can plausibly paste and return a fetchable URL.

    Stremio's install links use a stremio:// scheme, the Configure button
    often yields a page URL rather than the manifest, and people paste with
    stray whitespace. Being liberal here removes most of the ways this step
    can go wrong.
    """
    url = (url or "").strip().strip('"\'')
    if not url:
        return None
    if url.startswith("stremio://"):
        url = "https://" + url[len("stremio://"):]
    url = url.split("#")[0].rstrip("/")
    if not url.endswith("manifest.json"):
        url += "/manifest.json"
    if not url.startswith(("http://", "https://")):
        return None
    return url


def load_saved_addons():
    try:
        with open(ADDONS_PATH) as f:
            data = json.load(f)
        return [a for a in data.get("addons", []) if a]
    except Exception:
        return []


def save_saved_addons(urls):
    tmp = ADDONS_PATH + ".tmp"
    with open(tmp, "w") as f:
        json.dump({"addons": urls}, f, indent=2)
    os.replace(tmp, ADDONS_PATH)        # atomic; never a half-written file
    try:
        os.chmod(ADDONS_PATH, 0o600)    # addon URLs embed debrid credentials
    except Exception:
        pass

# ------------------------------------------------- stremio account


def api_post(method, payload, want_raw=False):
    body = json.dumps(payload).encode()
    req = urllib.request.Request(
        "%s/%s" % (STREMIO_API, method), data=body,
        headers={"Content-Type": "application/json",
                 "User-Agent": "vitastremio/1"})
    with urllib.request.urlopen(req, timeout=20) as r:
        data = json.loads(r.read().decode("utf-8", "replace"))
    if "error" in data and data["error"]:
        err = data["error"]
        raise RuntimeError(err.get("message") if isinstance(err, dict) else err)
    return data if want_raw else (data.get("result") or {})


def describe(obj, depth=0, maxd=3):
    """Render a JSON structure showing shape and types, not secrets.

    Printed when login fails so the actual response shape is visible instead
    of guessed at. Values are replaced by type and length, so this is safe to
    paste when asking for help.
    """
    pad = "  " * depth
    if isinstance(obj, dict):
        if depth >= maxd:
            return pad + "{...%d keys...}" % len(obj)
        out = []
        for k, v in obj.items():
            if isinstance(v, (dict, list)):
                out.append("%s%s:" % (pad, k))
                out.append(describe(v, depth + 1, maxd))
            else:
                t = type(v).__name__
                n = len(v) if isinstance(v, str) else ""
                out.append("%s%s: <%s%s>" % (pad, k, t,
                                             " len=%d" % n if n != "" else ""))
        return "\n".join(out)
    if isinstance(obj, list):
        if not obj:
            return pad + "[] (empty)"
        return pad + "[%d items]\n" % len(obj) + describe(obj[0], depth + 1, maxd)
    return pad + "<%s>" % type(obj).__name__


def load_auth_key():
    try:
        with open(AUTH_PATH) as f:
            return f.read().strip() or None
    except Exception:
        return None


def find_transport_urls(obj):
    """Every manifest URL anywhere in a response.

    The addon collection has been nested differently across API versions, so
    walking for the field is more durable than assuming result.addons[].
    """
    urls = []

    def walk(node):
        if isinstance(node, dict):
            for k, v in node.items():
                if k in ("transportUrl", "transport_url") \
                   and isinstance(v, str) and "manifest.json" in v:
                    urls.append(v)
                else:
                    walk(v)
        elif isinstance(node, list):
            for v in node:
                walk(v)
        elif isinstance(node, str):
            # Some versions return a bare list of URLs.
            if node.startswith("http") and node.endswith("manifest.json"):
                urls.append(node)

    walk(obj)
    return urls


# Method names and payloads the addon collection endpoint has used.
COLLECTION_VARIANTS = [
    ("AddonCollectionGet", {"type": "AddonCollectionGet", "update": True}),
    ("AddonCollectionGet", {"type": "AddonCollectionGet"}),
    ("AddonCollectionGet", {}),
    ("addonCollectionGet", {"type": "AddonCollectionGet"}),
]


def fetch_account_addons(auth_key):
    """Pull the account's installed addons, config strings included."""
    last = {}
    for method, extra in COLLECTION_VARIANTS:
        payload = dict(extra)
        payload["authKey"] = auth_key
        try:
            raw = api_post(method, payload, want_raw=True)
        except Exception as e:
            last = {"error": str(e)}
            continue

        last = raw
        urls = dedupe(find_transport_urls(raw))
        if urls:
            print("[mw] addon collection via %s -> %d addons"
                  % (method, len(urls)))
            return urls

    set_addon_shape(describe(last))
    print("[mw] no addons found in the collection response; shape was:")
    for line in describe(last).split("\n"):
        print("    %s" % line)
    set_login_shape(describe(last))
    return []


def ensure_catalog_provider(urls):
    """Guarantee something in the list can serve catalogs.

    Cinemeta is a default Stremio addon so the account normally includes it,
    but an account that removed it -- or an API response shaped differently
    than expected -- would leave only stream providers, and the Vita would
    show an empty grid with no error anywhere. Appending the fallback is
    harmless when it's already there (deduplicated below) and saves a
    confusing debugging session when it isn't.
    """
    if any("cinemeta" in u.lower() for u in urls):
        return urls
    print("[mw] no catalog provider in the addon list, adding Cinemeta")
    return urls + list(FALLBACK_ADDONS)


def dedupe(urls):
    seen, out = set(), []
    for u in urls:
        if u not in seen:
            seen.add(u)
            out.append(u)
    return out


def resolve_addons():
    """ADDONS env > Stremio account > Cinemeta only.

    The env var wins so you can test a single addon in isolation without
    logging out.
    """
    env = os.environ.get("ADDONS", "").strip()
    if env:
        print("[mw] using ADDONS from environment")
        return dedupe([a.strip() for a in env.split(",") if a.strip()])

    saved = load_saved_addons()
    if saved:
        print("[mw] using %d addons from %s" % (len(saved), ADDONS_PATH))
        return dedupe(ensure_catalog_provider(saved))

    key = load_auth_key()
    if key:
        try:
            urls = fetch_account_addons(key)
            if urls:
                print("[mw] loaded %d addons from Stremio account" % len(urls))
                return dedupe(ensure_catalog_provider(urls))
            print("[mw] account has no addons; falling back")
        except Exception as e:
            print("[mw] account addon fetch failed (%s)" % e)
            print("[mw] re-add the key: middleware.py --auth-key KEY, "
                  "or use the config page")

    print("[mw] no account or ADDONS set, using Cinemeta only "
          "(catalogs will work, streams will not)")
    return list(FALLBACK_ADDONS)


ADDONS = []          # populated in main(), mutated by the config page


# How often to re-pull the account's addon list. Installing an addon in
# Stremio then shows up on the Vita on its own, which is the whole point of
# linking the account rather than pasting URLs.
SYNC_INTERVAL = int(os.environ.get("SYNC_INTERVAL", "900"))


def sync_from_account(verbose=True):
    """Refresh the saved addon list from the linked Stremio account.

    No-op without an auth key. Failures are logged and swallowed: a flaky
    network must never leave the Vita with an empty addon list, so the last
    known-good list stays in place.
    """
    key = load_auth_key()
    if not key:
        return None
    try:
        urls = fetch_account_addons(key)
    except Exception as e:
        if verbose:
            print("[mw] account sync failed (%s); keeping current list" % e)
        return None
    if not urls:
        if verbose:
            print("[mw] account returned no addons; keeping current list")
        return None

    urls = dedupe(ensure_catalog_provider(urls))
    if urls != load_saved_addons():
        save_saved_addons(urls)
        reload_addons()
        print("[mw] account sync: %d addons (changed)" % len(urls))
    elif verbose:
        print("[mw] account sync: %d addons (no change)" % len(urls))
    return urls


def sync_loop():
    while True:
        time.sleep(SYNC_INTERVAL)
        try:
            sync_from_account(verbose=False)
        except Exception:
            pass


def reload_addons():
    global ADDONS
    ADDONS = resolve_addons()
    return ADDONS


CONFIG_PAGE = """<!doctype html><html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#0c0a14">
<title>vitastremio</title>
<style>
 *,*::before,*::after{box-sizing:border-box}
 :root{
   color-scheme:dark;
   --bg:#0c0a14; --panel:#16121f; --panel2:#1d1829; --line:#2a2338;
   --ink:#f2f0f7; --dim:#9a94ad; --faint:#6b6482;
   --accent:#7b5cff; --accent-dim:#5a46b4;
   --ok:#5ce09b; --warn:#e0b45c; --bad:#f0708c;
   --r:14px;
 }
 html{-webkit-text-size-adjust:100%}
 body{
   margin:0;background:var(--bg);color:var(--ink);
   font:15px/1.55 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,
        "Helvetica Neue",Arial,sans-serif;
   padding:0 18px calc(48px + env(safe-area-inset-bottom));
   max-width:760px;margin:0 auto;
   background-image:radial-gradient(900px 380px at 50% -160px,
                    rgba(123,92,255,.16),transparent 70%);
 }
 header{padding:34px 0 18px}
 h1{margin:0;font-size:23px;letter-spacing:-.4px;font-weight:650}
 h1 .dot{color:var(--accent)}
 .sub{color:var(--dim);font-size:13.5px;margin-top:5px}
 h2{font-size:11.5px;letter-spacing:.11em;text-transform:uppercase;
    color:var(--faint);font-weight:650;margin:30px 0 11px}

 .card{background:var(--panel);border:1px solid var(--line);
       border-radius:var(--r);padding:15px}
 .card + .card{margin-top:10px}

 .status{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));
         gap:9px}
 .stat{background:var(--panel);border:1px solid var(--line);
       border-radius:11px;padding:11px 13px}
 .stat .k{font-size:11px;color:var(--faint);text-transform:uppercase;
          letter-spacing:.07em}
 .stat .v{font-size:14.5px;margin-top:3px;display:flex;align-items:center;
          gap:7px;font-weight:550}
 .pip{width:8px;height:8px;border-radius:50%;flex:none}
 .pip.ok{background:var(--ok);box-shadow:0 0 0 3px rgba(92,224,155,.16)}
 .pip.bad{background:var(--bad);box-shadow:0 0 0 3px rgba(240,112,140,.16)}
 .pip.warn{background:var(--warn);box-shadow:0 0 0 3px rgba(224,180,92,.16)}

 .addon{display:flex;align-items:center;gap:13px}
 .avatar{width:38px;height:38px;border-radius:11px;flex:none;
         display:grid;place-items:center;font-weight:700;font-size:14px;
         background:linear-gradient(145deg,var(--accent),var(--accent-dim));
         color:#12101a}
 .meta{min-width:0;flex:1}
 .nm{font-weight:600;font-size:14.5px;overflow:hidden;text-overflow:ellipsis;
     white-space:nowrap}
 .url{font:12px/1.4 ui-monospace,SFMono-Regular,Menlo,monospace;
      color:var(--faint);overflow:hidden;text-overflow:ellipsis;
      white-space:nowrap;margin-top:2px}

 input{width:100%;padding:13px 14px;border-radius:11px;
       border:1px solid var(--line);background:#100d18;color:var(--ink);
       font-size:16px;outline:none;transition:border-color .15s,box-shadow .15s}
 input:focus{border-color:var(--accent);
             box-shadow:0 0 0 3px rgba(123,92,255,.18)}
 input::placeholder{color:#5a5470}

 button{border:0;border-radius:11px;padding:13px 17px;font-size:15px;
        font-weight:600;cursor:pointer;background:var(--accent);color:#fff;
        transition:filter .15s,transform .06s;width:100%}
 button:hover{filter:brightness(1.09)}
 button:active{transform:translateY(1px)}
 button.ghost{background:var(--panel2);color:var(--ink);
              border:1px solid var(--line)}
 button.tiny{width:auto;padding:8px 13px;font-size:13px;
             background:var(--panel2);color:var(--dim);
             border:1px solid var(--line)}
 button.tiny:hover{color:var(--bad);border-color:#4a2a38}

 .stack{display:flex;flex-direction:column;gap:10px}
 .row{display:flex;gap:10px;align-items:center}
 form{margin:0}

 .msg{padding:12px 15px;border-radius:12px;margin:16px 0;font-size:14px;
      display:flex;gap:10px;align-items:flex-start;
      animation:rise .22s ease-out}
 @keyframes rise{from{opacity:0;transform:translateY(-5px)}to{opacity:1}}
 .msg.ok{background:rgba(92,224,155,.1);color:#a8ecc8;
         border:1px solid rgba(92,224,155,.25)}
 .msg.err{background:rgba(240,112,140,.1);color:#f5aebd;
          border:1px solid rgba(240,112,140,.25)}

 .hint{color:var(--faint);font-size:12.5px;margin-top:9px;line-height:1.5}
 .empty{color:var(--dim);font-size:14px;text-align:center;padding:20px 0}
 code{font:12.5px ui-monospace,SFMono-Regular,Menlo,monospace;
      background:#100d18;padding:2px 6px;border-radius:5px;color:#c3bbdd}
 footer{margin-top:34px;padding-top:18px;border-top:1px solid var(--line);
        color:var(--faint);font-size:12px;display:flex;
        justify-content:space-between;gap:12px;flex-wrap:wrap}
 details summary{cursor:pointer;color:var(--dim);font-size:13.5px;
                 padding:4px 0;list-style:none}
 details summary::-webkit-details-marker{display:none}
 details summary::before{content:"+ ";color:var(--accent)}
 details[open] summary::before{content:"- "}
</style></head><body>

<header>
  <h1>vitastremio<span class="dot">.</span></h1>
  <div class="sub">@@STATUS@@</div>
</header>

@@MSG@@

<h2>Server</h2>
<div class="status">@@HEALTH@@</div>
<div class="hint">The Stremio server is only needed for <em>torrent</em>
  sources. Debrid and other direct links are fetched straight from the
  source, so "not used" is normal for those setups.</div>

<h2>Add an addon</h2>
<div class="card">
  <form method="post" action="/config/add" class="stack">
    <input name="url" placeholder="https://.../manifest.json"
           autocapitalize="off" autocorrect="off" spellcheck="false"
           inputmode="url">
    <button type="submit">Add addon</button>
  </form>
  <div class="hint">In Stremio, open the addon's <b>Configure</b> or
    <b>Share</b> link and paste it here &mdash; <code>stremio://</code>
    links work too. This is the most reliable way to set things up: an
    addon URL keeps working regardless of what changes upstream.<br><br>
    You need at least one <b>catalog</b> addon (Cinemeta, added
    automatically) and one <b>stream</b> addon (Torrentio, AIOStreams,
    Comet, and so on).</div>
</div>

<h2>Configured</h2>
@@ADDONS@@

<h2>Import from a Stremio account <span style="text-transform:none;
    letter-spacing:0;color:var(--faint);font-weight:400">&mdash; optional</span></h2>
<details style="margin-top:2px">
  <summary>Import with an auth key</summary>
  <div class="card" style="margin-top:9px">
    <form method="post" action="/config/authkey" class="stack">
      <input name="key" placeholder="authKey from app.strem.io"
             autocapitalize="off" autocorrect="off" spellcheck="false">
      <button type="submit">Import addons</button>
    </form>
    <div class="hint">
      Pulls in every addon on your account at once, then re-syncs every
      @@SYNCMIN@@ minutes.<br><br>
      Sign in at <code>app.strem.io</code>, open developer tools, then
      <b>Application &rarr; Local Storage</b> and copy the
      <code>authKey</code> value.<br><br>
      There is deliberately no email and password form here: Stremio's login
      endpoint is undocumented and its shape has changed more than once,
      which made it a maintenance burden that broke without warning. An auth
      key works the same way and keeps working.
    </div>
  </div>
  @@ADDONSHAPE@@
</details>

<details style="margin-top:26px">
  <summary>Reset</summary>
  <div class="card" style="margin-top:9px">
    <form method="post" action="/config/clear">
      <button type="submit" class="ghost">Remove all addons</button>
    </form>
    <div class="hint">Cinemeta is re-added automatically so the catalog
      keeps working.</div>
  </div>
</details>

<footer>
  <span>Point the Vita at <code>@@HOSTPORT@@</code></span>
  <span>v1.0 beta</span>
</footer>
</body></html>"""


def render_config(msg="", err=""):
    rows = []
    for u in ADDONS:
        try:
            host = u.split("/")[2]
        except Exception:
            host = u
        # Two letters from the host make a recognisable avatar without
        # fetching anything: "to" for torrentio, "v3" for cinemeta.
        label = re.sub(r"^(www\.|v\d+-)", "", host)
        initials = label[:2].upper()

        rows.append(
            '<div class="card addon">'
            '<div class="avatar">%s</div>'
            '<div class="meta"><div class="nm">%s</div>'
            '<div class="url">%s</div></div>'
            '<form method="post" action="/config/remove">'
            '<input type="hidden" name="url" value="%s">'
            '<button class="tiny" type="submit">Remove</button>'
            '</form></div>'
            % (html_escape(initials), html_escape(label),
               html_escape(u[:110]), html_escape(u)))

    if not rows:
        rows.append('<div class="card"><div class="empty">'
                    'No addons yet &mdash; add one below.</div></div>')

    banner = ""
    if err:
        banner = '<div class="msg err"><span>!</span><span>%s</span></div>' \
                 % html_escape(err)
    elif msg:
        banner = '<div class="msg ok"><span>&check;</span><span>%s</span></div>' \
                 % html_escape(msg)

    has_stream = any("cinemeta" not in a.lower() for a in ADDONS)
    status = ("%d addon%s configured" % (len(ADDONS),
                                         "" if len(ADDONS) == 1 else "s")) + (
             "" if has_stream else " - catalogs only, no stream sources yet")

    # Health panel. Everything here is cheap to determine; the Stremio check
    # has a short timeout so a dead server cannot hang the page.
    def stat(k, v, pip):
        return ('<div class="stat"><div class="k">%s</div>'
                '<div class="v"><span class="pip %s"></span>%s</div></div>'
                % (html_escape(k), pip, html_escape(v)))

    reachable = stremio_reachable()
    # Only torrent sources route through it; direct/debrid links are fetched
    # by ffmpeg itself. Calling it a failure would be wrong for anyone whose
    # addons return resolved links.

    # "unreachable" is only a fault if torrent sources are actually in use.
    # Addons that return resolved links -- any debrid setup -- are fetched by
    # ffmpeg directly and never touch the Stremio server, so flagging it red
    # sends people to debug something they do not need.
    if reachable:
        stremio_state, stremio_pip = "connected", "ok"
    else:
        stremio_state, stremio_pip = "not in use", "warn"
    if VAAPI_OK is None:
        vaapi = ("not probed", "warn")
    elif VAAPI_OK:
        vaapi = ("hardware", "ok")
    else:
        vaapi = ("software (slow)", "warn")

    linked = bool(load_auth_key())
    health = "".join([
        stat("Stremio server", stremio_state, stremio_pip),
        stat("Transcoding", vaapi[0], vaapi[1]),
        stat("Stream sources",
             "ready" if has_stream else "none configured",
             "ok" if has_stream else "warn"),
        stat("Account", "linked" if linked else "not linked",
             "ok" if linked else "warn"),
    ])

    ashape = ""
    if LAST_ADDON_SHAPE:
        ashape = ('<div class="card" style="margin-top:10px">'
                  '<div style="font-size:11px;color:var(--faint);'
                  'text-transform:uppercase;letter-spacing:.07em">'
                  'Account returned no addons &mdash; response shape</div>'
                  '<pre style="margin:9px 0 0;overflow-x:auto;font:12px '
                  'ui-monospace,Menlo,monospace;color:#c3bbdd">%s</pre>'
                  '<div class="hint">Either the account has no addons '
                  'installed, or the API shape moved again. Adding addons by '
                  'URL below works regardless.</div></div>'
                  % html_escape(LAST_ADDON_SHAPE))

    page = (CONFIG_PAGE
            .replace("@@ADDONSHAPE@@", ashape)
            .replace("@@ADDONS@@", "\n".join(rows))
            .replace("@@MSG@@", banner)
            .replace("@@HEALTH@@", health)
            .replace("@@STATUS@@", html_escape(status))
            .replace("@@HOSTPORT@@", "&lt;this-host&gt;:%d" % LISTEN_PORT)
            .replace("@@SYNCMIN@@", str(max(1, SYNC_INTERVAL // 60))))
    return page.encode("utf-8")


def html_escape(t):
    return (str(t).replace("&", "&amp;").replace("<", "&lt;")
            .replace(">", "&gt;").replace('"', "&quot;"))

US = b"\x1f"   # field separator
RS = b"\n"     # record separator

# ---------------------------------------------------------------- helpers

# Catalog/meta lookups are fast lookups against a metadata cache. Stream
# lookups are not: an aggregator like AIOStreams fans out to many scrapers
# and debrid services, and 30-60s on a cold cache is normal. One shared
# timeout would either abandon real stream results or make browsing sluggish.
TIMEOUT_META   = 12
TIMEOUT_STREAM = int(os.environ.get("STREAM_TIMEOUT", "75"))

# Aggregators can return hundreds of streams. The Vita shows 12 and stores
# 60, so trimming here saves bandwidth and parse time on the handheld.
MAX_STREAMS = int(os.environ.get("MAX_STREAMS", "40"))


def fetch_json(url, timeout=TIMEOUT_META):
    req = urllib.request.Request(url, headers={"User-Agent": "vitastremio/1"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8", "replace"))


def addon_base(manifest_url):
    return manifest_url.rsplit("/manifest.json", 1)[0]


def vtype(t):
    """ctype is interpolated raw into an addon URL path, so constrain it."""
    return t if t in VALID_TYPES else "movie"


def sanitize(s):
    """Strip separators and non-latin1 so the Vita's font doesn't choke."""
    if s is None:
        return ""
    s = str(s).replace("\x1f", " ").replace("\n", " ").replace("\r", " ")
    return s.encode("latin-1", "replace").decode("latin-1")


def pack(rows):
    out = bytearray()
    for row in rows:
        out += US.join(sanitize(f).encode("latin-1") for f in row) + RS
    return bytes(out)


# ---------------------------------------------------------------- stream keys
# A "stream key" is whatever the addon handed us -- usually an infoHash plus
# file index, sometimes a direct URL. We base64 it so it survives a query
# string, and resolve it back to something ffmpeg can open.

import base64


def key_encode(stream_obj):
    if stream_obj.get("url"):
        raw = "u|" + stream_obj["url"]
    else:
        ih = stream_obj.get("infoHash", "")
        idx = stream_obj.get("fileIdx", 0)
        raw = "t|%s|%s" % (ih, idx if idx is not None else 0)
    return base64.urlsafe_b64encode(raw.encode()).decode().rstrip("=")


def key_decode(key):
    pad = "=" * (-len(key) % 4)
    raw = base64.urlsafe_b64decode(key + pad).decode()
    kind, _, rest = raw.partition("|")
    if kind == "u":
        return rest
    ih, _, idx = rest.partition("|")
    # Stremio server exposes torrent files at /{infoHash}/{fileIdx}
    return "%s/%s/%s" % (STREMIO, ih, idx or "0")


# ---------------------------------------------------------------- ffmpeg

# How far before the target to begin the coarse seek. Long enough that the
# fine seek always has a keyframe to work from, short enough that the extra
# decoding is not noticeable.
SEEK_PREROLL = 8.0


def seek_args(offset):
    """Split a seek into a coarse input seek and a fine output seek.

    Input seeking alone lands video on the nearest keyframe and audio on the
    nearest audio frame, which are not the same instant -- and the two
    ffmpeg processes make that choice independently, so every seek shifted
    A/V by a different amount.

    Seeking coarsely on the input and then trimming precisely on the output
    makes both processes land on exactly the requested timestamp, because
    the output seek is relative to timestamps both share. The cost is
    decoding up to SEEK_PREROLL seconds that get discarded.

    Returns (input_args, output_args).
    """
    try:
        t = float(offset or 0)
    except (TypeError, ValueError):
        t = 0.0
    if t <= 0.01:
        return [], []

    coarse = max(0.0, t - SEEK_PREROLL)
    fine   = t - coarse
    out    = ["-ss", "%.3f" % fine] if fine > 0.01 else []
    return (["-ss", "%.3f" % coarse] if coarse > 0.01 else []), out


def probe_audio_tracks(src):
    """List the source's audio streams.

    Returns [(ord, lang, codec, channels, title)] where ord is the index
    among audio streams, which is what -map 0:a:N wants. Reads only the
    container header, so it is quick even on a remote source.
    """
    cmd = [
        "ffprobe", "-v", "error",
        "-select_streams", "a",
        "-show_entries",
        "stream=index,codec_name,channels:stream_tags=language,title",
        "-of", "json",
        src,
    ]
    try:
        r = subprocess.run(cmd, capture_output=True, timeout=45)
        data = json.loads(r.stdout.decode("utf-8", "replace") or "{}")
    except Exception as e:
        print("[mw] audio probe failed (%s)" % e)
        return []

    out = []
    for n, st in enumerate(data.get("streams", [])):
        tags = st.get("tags") or {}
        out.append((
            n,
            (tags.get("language") or "").lower(),
            st.get("codec_name") or "",
            st.get("channels") or 0,
            (tags.get("title") or "")[:40],
        ))
    return out


def probe_stream(src):
    """Return (num, den, milli, duration_s) for the source.

    One ffprobe for both: the frame rate the client needs for pacing, and the
    duration it needs to draw a meaningful progress bar.

    Padding 23.976 film up to 30fps duplicates frames on an uneven cadence,
    which reads as judder on every pan. Matching the source rate removes it
    entirely. Falls back to 30 if the probe fails, which is the old
    behaviour -- correct, just less smooth.
    """
    cmd = [
        "ffprobe", "-v", "error",
        "-select_streams", "v:0",
        "-show_entries", "stream=r_frame_rate:format=duration",
        "-of", "default=nw=1:nk=1",
        src,
    ]
    duration = 0
    try:
        r = subprocess.run(cmd, capture_output=True, timeout=30)
        lines = [x for x in r.stdout.decode("utf-8", "replace").split() if x]

        # Duration comes from the format section and may be absent or "N/A"
        # on a live source; the client treats 0 as unknown.
        for tok in lines[1:]:
            try:
                duration = int(float(tok))
                break
            except ValueError:
                pass

        num, _, den = lines[0].partition("/")
        num, den = int(num), int(den or 1)
        if den <= 0 or num <= 0:
            raise ValueError(lines[0])
        milli = int(round(num * 1000.0 / den))
        # Sanity band: anything outside this is a bad probe, not a real rate.
        if not (10000 <= milli <= 61000):
            raise ValueError("implausible rate %s" % milli)
        return num, den, milli, duration
    except Exception as e:
        print("[mw] stream probe failed (%s), assuming 30fps" % e)
        return 30, 1, 30000, duration


def video_cmd(src, offset, fps=(30, 1)):
    """VAAPI decode -> scale -> H.264 encode -> raw Annex-B on stdout.

    -hwaccel_output_format is deliberately NOT set. Keeping frames in system
    memory costs a little bandwidth but survives 10-bit HEVC sources, which
    otherwise trip scale_vaapi with a p010 format error. The encode is still
    fully on the GPU, which is where the cost actually is.
    """
    pre, post = seek_args(offset)
    return [
        "ffmpeg", "-loglevel", "error", "-nostdin",
        "-vaapi_device", RENDER_NODE,
    ] + pre + [
        "-i", src,
    ] + post + [
        "-an",
        "-vf", "scale=%d:%d:force_original_aspect_ratio=decrease,"
               "pad=%d:%d:(ow-iw)/2:(oh-ih)/2,format=nv12,hwupload"
               % (VID_W, VID_H, VID_W, VID_H),
        "-c:v", "h264_vaapi",
        "-profile:v", "main",
        "-level", "31",            # VAAPI wants 31, not 3.1
        # Constant frame rate is REQUIRED: the client derives presentation
        # time from a frame counter, so a variable rate would drift against
        # the audio clock. But the rate now MATCHES THE SOURCE rather than
        # being forced to 30 -- padding 23.976 film to 30 duplicates frames
        # unevenly and that cadence is visible as judder on any pan.
        "-vsync", "cfr", "-r", "%d/%d" % (fps[0], fps[1]),
        # Pinned to match REF_FRAMES in player.c. A larger reference window
        # would need a bigger phycont allocation on the Vita, which is the
        # allocation most likely to fail.
        "-refs", "3",
        # No B-frames. The Vita client tags each decoded picture with the
        # timestamp of the access unit it just submitted, which is only valid
        # when output order matches input order. B-frames reorder, so every
        # picture would carry the wrong timestamp and the frame scheduler
        # would pace against nonsense -- jerky playback with all the pacing
        # machinery working correctly. Also cuts decoder latency.
        "-bf", "0",
        "-g", str(max(12, int(round(fps[0] / float(fps[1]) * 2)))),
        "-b:v", VID_BITRATE,
        "-maxrate", VID_MAXRATE,
        "-bufsize", "6M",
        "-bsf:v", "h264_mp4toannexb",
        "-f", "h264", "-",
    ]


def audio_cmd(src, offset, track=-1):
    pre, post = seek_args(offset)
    return [
        "ffmpeg", "-loglevel", "error", "-nostdin",
    ] + pre + [
        "-i", src,
    ] + post + (
        # With an explicit map, selection is already unambiguous, so -vn is
        # redundant. The trailing "?" makes the map optional: a stale or
        # out-of-range index then yields no audio rather than aborting the
        # whole command, which is the difference between quiet playback and
        # the stream dying outright.
        ["-map", "0:a:%d?" % track] if track >= 0 else ["-vn"]
    ) + [
        "-af", "aresample=async=1",
        "-c:a", "pcm_s16le",
        "-ar", str(AUD_RATE),
        "-ac", str(AUD_CH),
        "-f", "s16le", "-",
    ]


def stream_process(handler, cmd, content_type, fps_milli=0, duration=0):
    handler.send_response(200)
    handler.send_header("Content-Type", content_type)
    if fps_milli:
        # The client times frames from this, so 23.976 must arrive as 23976
        # rather than rounded -- rounding to 24 drifts ~3.6s per hour.
        handler.send_header("X-Video-FPS", str(fps_milli))
    if duration:
        handler.send_header("X-Duration", str(duration))
    handler.send_header("Cache-Control", "no-store")
    # No Content-Length is possible on a live transcode, so the response must
    # be delimited by connection close. Being explicit stops the HTTP/1.1
    # handler from attempting keep-alive on an unbounded body.
    handler.send_header("Connection", "close")
    handler.end_headers()
    handler.close_connection = True

    if not TRANSCODE_SLOTS.acquire(timeout=5):
        handler.log_message("refusing stream: %d transcodes already running",
                            MAX_TRANSCODES)
        return

    proc = subprocess.Popen(
        cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        preexec_fn=os.setsid,
    )

    # Drain stderr on a thread. Without this ffmpeg blocks forever once the
    # pipe buffer fills, and every failure looks identical from the Vita.
    errbuf = []

    def drain_err():
        for line in iter(proc.stderr.readline, b""):
            errbuf.append(line.decode("utf-8", "replace").rstrip())
            if len(errbuf) > 40:
                del errbuf[0]

    threading.Thread(target=drain_err, daemon=True).start()

    sent = 0
    try:
        while True:
            chunk = proc.stdout.read(32768)
            if not chunk:
                break
            handler.wfile.write(chunk)
            sent += len(chunk)
    except (BrokenPipeError, ConnectionResetError):
        pass          # Vita closed the socket -- normal on stop/seek
    finally:
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        except Exception:
            pass
        proc.wait()
        TRANSCODE_SLOTS.release()
        # A stream that produced almost nothing means ffmpeg failed, not that
        # the user pressed stop. Surface why.
        if sent < 65536 and errbuf:
            handler.log_message("ffmpeg produced only %d bytes; last output:",
                                sent)
            for line in errbuf[-8:]:
                handler.log_message("    %s", line)


# ---------------------------------------------------------------- posters

# Posters are small but come from a third-party CDN that is sometimes slow.
# The client retries a few times, so a failure here is recoverable -- but
# each failure costs the user a blank cell for several seconds, so it is
# worth being patient and caching what succeeds.
POSTER_TIMEOUT = int(os.environ.get("POSTER_TIMEOUT", "25"))
POSTER_CACHE_MAX = 400
_poster_cache = {}
_poster_lock = threading.Lock()


def resize_poster(url):
    """Shell out to ffmpeg rather than depend on Pillow."""
    with _poster_lock:
        hit = _poster_cache.get(url)
    if hit is not None:
        return hit

    cmd = [
        "ffmpeg", "-loglevel", "error", "-nostdin",
        # Follow redirects and wait a little for the CDN; the defaults give
        # up quickly enough to fail on an otherwise fine image.
        "-reconnect", "1", "-reconnect_streamed", "1",
        "-rw_timeout", "15000000",
        "-i", url,
        "-vf", "scale=%d:%d" % (POSTER_W, POSTER_H),
        "-frames:v", "1", "-q:v", "6",
        "-f", "mjpeg", "-",
    ]
    out = b""
    try:
        out = subprocess.run(cmd, capture_output=True,
                             timeout=POSTER_TIMEOUT).stdout
    except Exception as e:
        print("[mw] poster ffmpeg error (%s): %s" % (e, url[:70]))

    if len(out) <= 128:
        # Fall back to fetching with urllib and handing ffmpeg a local file.
        #
        # ffmpeg's HTTP client is fussy in ways Python's is not: some image
        # CDNs reject its user-agent, and its redirect and TLS handling are
        # less forgiving. When a poster fails consistently while the URL is
        # perfectly good in a browser, this is usually why.
        try:
            req = urllib.request.Request(url, headers={
                "User-Agent": "Mozilla/5.0 (compatible; vitastremio/1)",
                "Accept": "image/*,*/*",
            })
            with urllib.request.urlopen(req, timeout=POSTER_TIMEOUT) as r:
                raw = r.read(8 * 1024 * 1024)

            tmp = "/tmp/vitastremio_poster_%d" % threading.get_ident()
            with open(tmp, "wb") as f:
                f.write(raw)
            try:
                out = subprocess.run(
                    ["ffmpeg", "-loglevel", "error", "-nostdin", "-i", tmp,
                     "-vf", "scale=%d:%d" % (POSTER_W, POSTER_H),
                     "-frames:v", "1", "-q:v", "6", "-f", "mjpeg", "-"],
                    capture_output=True, timeout=POSTER_TIMEOUT).stdout
            finally:
                try:
                    os.remove(tmp)
                except OSError:
                    pass

            if len(out) > 128:
                print("[mw] poster recovered via urllib: %s" % url[:70])
        except Exception as e:
            print("[mw] poster fallback failed (%s): %s" % (e, url[:70]))

    if len(out) > 128:
        with _poster_lock:
            # Bounded, and a plain dict keeps insertion order, so dropping
            # the oldest key is enough of an eviction policy here.
            if len(_poster_cache) >= POSTER_CACHE_MAX:
                _poster_cache.pop(next(iter(_poster_cache)))
            _poster_cache[url] = out
    else:
        print("[mw] poster produced %d bytes: %s" % (len(out), url[:70]))
    return out


# ---------------------------------------------------------------- handler

class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    timeout = 120          # reap sockets left behind by a sleeping Vita

    def log_message(self, fmt, *args):
        sys.stderr.write("[mw] " + (fmt % args) + "\n")

    def _send(self, body, ctype="text/plain; charset=latin-1", code=200):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _redirect(self, msg="", err=""):
        """POST-then-redirect so a refresh doesn't resubmit the form."""
        q = []
        if msg: q.append("msg=" + urllib.parse.quote(msg))
        if err: q.append("err=" + urllib.parse.quote(err))
        self.send_response(303)
        self.send_header("Location", "/?" + "&".join(q))
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_POST(self):
        parsed = urllib.parse.urlparse(self.path)
        length = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(length).decode("utf-8", "replace")
        form = urllib.parse.parse_qs(raw)
        field = lambda k: (form.get(k) or [""])[0]

        try:
            if parsed.path == "/config/add":
                url = normalize_manifest(field("url"))
                if not url:
                    return self._redirect(err="That doesn't look like a URL.")
                # Fetch it now: a typo caught here beats an empty grid later.
                try:
                    manifest = fetch_json(url)
                    name = manifest.get("name", "addon")
                except Exception as e:
                    return self._redirect(
                        err="Couldn't load that addon (%s)" % e)

                saved = load_saved_addons() or list(ADDONS)
                if url in saved:
                    return self._redirect(err="%s is already added." % name)
                saved.append(url)
                save_saved_addons(saved)
                reload_addons()
                return self._redirect(msg="Added %s." % name)

            if parsed.path == "/config/remove":
                url = field("url")
                saved = [a for a in (load_saved_addons() or list(ADDONS))
                         if a != url]
                save_saved_addons(saved)
                reload_addons()
                return self._redirect(msg="Removed.")

            if parsed.path == "/config/sync":
                urls = sync_from_account()
                if urls is None:
                    return self._redirect(
                        err="No linked account, or the sync failed.")
                return self._redirect(msg="Synced %d addons." % len(urls))

            if parsed.path == "/config/clear":
                save_saved_addons([])
                reload_addons()
                return self._redirect(msg="Cleared.")

            if parsed.path == "/config/authkey":
                key = field("key").strip()
                if len(key) < 8:
                    return self._redirect(err="That does not look like a key.")

                with open(AUTH_PATH, "w") as f:
                    f.write(key)
                os.chmod(AUTH_PATH, 0o600)

                try:
                    urls = fetch_account_addons(key)
                except Exception as e:
                    return self._redirect(
                        err="Key saved, but fetching addons failed: %s" % e)
                if not urls:
                    return self._redirect(
                        err="Key saved, but the account returned no addons.")

                save_saved_addons(dedupe(ensure_catalog_provider(urls)))
                reload_addons()
                return self._redirect(msg="Imported %d addons." % len(urls))

            self._send(b"not found\n", code=404)
        except Exception as e:
            self.log_message("config error on %s: %s", parsed.path, e)
            self._redirect(err=str(e)[:160])

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(parsed.query)
        one = lambda k, d="": (q.get(k) or [d])[0]

        try:
            if parsed.path == "/catalog":
                self._send(self.catalog(vtype(one("type", "movie")),
                                        one("genre")))
            elif parsed.path == "/audiotracks":
                src   = key_decode(one("s"))
                rows  = []
                for n, lang, codec, ch, title in probe_audio_tracks(src):
                    code = Handler.ISO3.get(lang[:3], lang[:2].upper() or "??")
                    desc = codec.upper()
                    if ch:
                        desc += " %s" % ("5.1" if ch == 6 else
                                         "7.1" if ch == 8 else
                                         "stereo" if ch == 2 else
                                         "mono" if ch == 1 else "%dch" % ch)
                    if title:
                        desc += " - " + title
                    rows.append([str(n), code, desc[:44]])
                self.log_message("audio tracks -> %d", len(rows))
                self._send(pack(rows))
            elif parsed.path == "/subtracks":
                self._send(self.subtracks(vtype(one("type", "movie")),
                                          one("id")))
            elif parsed.path == "/subs":
                self._send(self.subs(one("s")))
            elif parsed.path == "/search":
                self._send(self.search(vtype(one("type", "movie")),
                                       one("q")))
            elif parsed.path == "/meta":
                self._send(self.meta(vtype(one("type", "movie")), one("id")))
            elif parsed.path == "/streams":
                self._send(self.streams(vtype(one("type", "movie")),
                                        one("id")))
            elif parsed.path == "/poster":
                self._send(resize_poster(one("u")), "image/jpeg")
            elif parsed.path == "/v":
                src = key_decode(one("s"))
                num, den, milli, dur = probe_stream(src)
                self.log_message("source rate %d/%d (%d milli-fps), %ds",
                                 num, den, milli, dur)
                stream_process(self, video_cmd(src, one("t", "0"),
                                               (num, den)),
                               "video/h264", milli, dur)
            elif parsed.path == "/a":
                src = key_decode(one("s"))
                try:
                    track = int(one("a", "-1"))
                except ValueError:
                    track = -1
                if track >= 0:
                    self.log_message("audio track %d requested", track)
                stream_process(self, audio_cmd(src, one("t", "0"), track),
                               "audio/L16")
            elif parsed.path in ("/", "/config"):
                self._send(render_config(one("msg"), one("err")),
                           "text/html; charset=utf-8")
            elif parsed.path == "/sync":
                # GET so the Vita's minimal HTTP client can call it.
                urls = sync_from_account()
                if urls is None:
                    self._send(b"nosync\n")
                else:
                    self._send(("ok %d\n" % len(urls)).encode())
            elif parsed.path == "/ping":
                # Include the pid so the Vita can tell a restarted middleware
                # from a still-running one and drop stale assumptions.
                self.send_response(200)
                body = b"ok\n"
                self.send_header("Content-Type", "text/plain")
                self.send_header("X-Pid", str(os.getpid()))
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            else:
                self._send(b"not found\n", code=404)
        except Exception as e:
            self.log_message("error on %s: %s", self.path, e)
            try:
                self._send(("err\x1f%s\n" % e).encode("latin-1", "replace"),
                           code=500)
            except Exception:
                pass

    # ------------------------------------------------------------ routes

    def catalog(self, ctype, genre):
        """Fields: id, name, year, posterURL"""
        rows = []
        for man in ADDONS:
            base = addon_base(man)
            url = "%s/catalog/%s/top.json" % (base, ctype)
            if genre:
                url = "%s/catalog/%s/top/genre=%s.json" % (
                    base, ctype, urllib.parse.quote(genre))
            try:
                data = fetch_json(url)
            except Exception:
                continue
            missing = 0
            for m in data.get("metas", []):
                poster = m.get("poster") or ""
                if not poster:
                    missing += 1
                rows.append([
                    m.get("id", ""),
                    m.get("name", ""),
                    str(m.get("year", "") or m.get("releaseInfo", "") or ""),
                    poster,
                ])
            if missing:
                self.log_message("catalog: %d of %d entries have no poster "
                                 "in the addon metadata",
                                 missing, len(data.get("metas", [])))
        return pack(rows)

    def search(self, ctype, query):
        """Same row format as /catalog so the client reuses one code path.

        Stremio exposes search as an 'extra' on a catalog, not a separate
        resource: /catalog/{type}/{catalogId}/search={q}.json. Which catalog
        ids accept it varies by addon, so we try each addon's declared
        catalogs and quietly skip the ones that 404.
        """
        rows = []
        query = (query or "").strip()
        if not query:
            return b""

        for man in ADDONS:
            base = addon_base(man)

            # Ask the manifest which catalogs advertise search rather than
            # guessing ids -- guessing 'top' works for Cinemeta and fails
            # silently everywhere else.
            catalog_ids = []
            try:
                manifest = fetch_json(man)
                for c in manifest.get("catalogs", []):
                    if c.get("type") != ctype:
                        continue
                    extras = c.get("extra") or []
                    names = {e.get("name") for e in extras if isinstance(e, dict)}
                    if "search" in names or "search" in (c.get("extraSupported") or []):
                        catalog_ids.append(c.get("id"))
            except Exception:
                pass
            if not catalog_ids:
                catalog_ids = ["top"]          # Cinemeta's default

            for cid in catalog_ids:
                url = "%s/catalog/%s/%s/search=%s.json" % (
                    base, ctype, cid, urllib.parse.quote(query))
                try:
                    data = fetch_json(url)
                except Exception:
                    continue
                for m in data.get("metas", []):
                    rows.append([
                        m.get("id", ""),
                        m.get("name", ""),
                        str(m.get("year", "") or m.get("releaseInfo", "") or ""),
                        m.get("poster", ""),
                    ])
                if rows:
                    break

        # Same id can come from several addons; keep first occurrence.
        seen, dedup = set(), []
        for r in rows:
            if r[0] in seen:
                continue
            seen.add(r[0])
            dedup.append(r)

        self.log_message("search %r -> %d results", query, len(dedup))
        return pack(dedup)

    def meta(self, ctype, mid):
        """First row: name, year, runtime, description.
        Subsequent rows (series only): videoId, season, episode, title"""
        rows = []
        for man in ADDONS:
            base = addon_base(man)
            try:
                data = fetch_json("%s/meta/%s/%s.json"
                                  % (base, ctype, urllib.parse.quote(mid)))
            except Exception:
                continue
            m = data.get("meta")
            if not m:
                continue
            desc = (m.get("description") or "")[:600]
            rows.append([m.get("name", ""), str(m.get("year", "")),
                         str(m.get("runtime", "")), desc])
            for v in (m.get("videos") or []):
                rows.append([v.get("id", ""), str(v.get("season", "")),
                             str(v.get("episode", "")), v.get("title", "")])
            break
        return pack(rows)

    # --- stream label parsing -------------------------------------------
    #
    # Addons pack everything into two multi-line strings meant for a rich
    # client: name is "Provider\nQuality", title is the release name, then a
    # line of emoji-tagged facts, then flag emoji for languages. The Vita
    # renders a PGF bitmap font with no emoji and limited width, so the
    # useful move is to pull the facts out here and send them as separate
    # fields the client can lay out itself.

    QUALITY_RE = re.compile(
        r"\b(2160p|1080p|720p|480p|4k|uhd|hdrip|cam|ts)\b", re.I)
    SIZE_RE    = re.compile(r"(\d+(?:[.,]\d+)?)\s*(TB|GB|MB|GiB|MiB)", re.I)
    SEED_RE    = re.compile(r"\U0001F464\s*(\d+)")
    PROV_RE    = re.compile(r"\u2699\ufe0f?\s*([^\s|/\n]+)")
    TAG_WORDS  = ("REMUX", "BluRay", "WEB-DL", "WEBRip", "HDR10+", "HDR",
                  "DV", "DoVi", "HEVC", "x265", "H265", "x264", "H264",
                  "Atmos", "TrueHD", "DTS-HD", "DTS", "AV1", "10bit")

    @staticmethod
    def _flags_to_codes(text):
        """Regional-indicator pairs -> ISO letters, e.g. two chars -> 'GB'."""
        out, buf = [], []
        for ch in text:
            o = ord(ch)
            if 0x1F1E6 <= o <= 0x1F1FF:
                buf.append(chr(o - 0x1F1E6 + ord("A")))
                if len(buf) == 2:
                    code = "".join(buf)
                    if code not in out:
                        out.append(code)
                    buf = []
            else:
                buf = []
        return out

    @classmethod
    def parse_stream(cls, st):
        """Flatten one addon stream into display fields."""
        name  = (st.get("name") or "").strip()
        title = (st.get("title") or "").strip()
        blob  = name + "\n" + title

        name_lines  = [l for l in name.split("\n") if l.strip()]
        title_lines = [l for l in title.split("\n") if l.strip()]

        provider = name_lines[0].strip() if name_lines else "stream"
        release  = title_lines[0].strip() if title_lines else provider

        m = cls.QUALITY_RE.search(" ".join(name_lines[1:]) or "")
        if not m:
            m = cls.QUALITY_RE.search(release)
        quality = m.group(1).lower().replace("4k", "2160p") if m else ""

        m = cls.SIZE_RE.search(blob)
        size = ("%s %s" % (m.group(1).replace(",", "."),
                           m.group(2).upper().replace("I", ""))) if m else ""

        m = cls.SEED_RE.search(blob)
        seeds = m.group(1) if m else ""

        m = cls.PROV_RE.search(blob)
        tracker = m.group(1).strip() if m else ""

        langs = cls._flags_to_codes(blob)

        upper = release.upper()
        tags = [t for t in cls.TAG_WORDS if t.upper() in upper]
        # HDR is implied by HDR10+ and DV by DoVi; keep the row short.
        if "HDR10+" in tags and "HDR" in tags:
            tags.remove("HDR")
        if "DoVi" in tags and "DV" in tags:
            tags.remove("DV")

        cached = ("instant" in blob.lower() or "cached" in blob.lower()
                  or bool(st.get("url")))

        return {
            "release":  release,
            "quality":  quality,
            "size":     size,
            "seeds":    seeds,
            "provider": provider,
            "tracker":  tracker,
            "langs":    " ".join(langs[:4]),
            "tags":     " ".join(tags[:4]),
            "cached":   cached,
        }

    # Flag emoji are regional-indicator pairs, so they decode to an ISO
    # country code arithmetically. The Vita's PGF font has no emoji, so they
    # have to become text or the language information is simply lost.
    FLAG_LANG = {
        "GB": "EN", "US": "EN", "ES": "ES", "MX": "ES", "FR": "FR",
        "DE": "DE", "IT": "IT", "PT": "PT", "BR": "PT", "RU": "RU",
        "JP": "JA", "KR": "KO", "CN": "ZH", "TW": "ZH", "IN": "HI",
        "NL": "NL", "PL": "PL", "SE": "SV", "NO": "NO", "DK": "DA",
        "FI": "FI", "TR": "TR", "GR": "EL", "SA": "AR", "IL": "HE",
        "TH": "TH", "VN": "VI", "ID": "ID", "HU": "HU", "CZ": "CS",
        "RO": "RO", "UA": "UK",
    }

    # Written as word-boundary patterns so "ITA" does not match inside
    # "DIGITAL" and "FRE" does not match "FREE".
    LANG_TOKENS = [
        (r"\b(eng|english)\b",                    "EN"),
        (r"\b(lat|latino|spa|esp|spanish|castellano)\b", "ES"),
        (r"\b(ita|italian)\b",                    "IT"),
        (r"\b(fre|fra|french|vff|vfq|truefrench)\b", "FR"),
        (r"\b(ger|deu|german)\b",                 "DE"),
        (r"\b(por|portuguese|dublado)\b",         "PT"),
        (r"\b(rus|russian)\b",                    "RU"),
        (r"\b(hin|hindi)\b",                      "HI"),
        (r"\b(tam|tamil)\b",                      "TA"),
        (r"\b(tel|telugu)\b",                     "TE"),
        (r"\b(jpn|jap|japanese)\b",               "JA"),
        (r"\b(kor|korean)\b",                     "KO"),
        (r"\b(chi|chs|cht|chinese|mandarin)\b",   "ZH"),
        (r"\b(dut|nld|dutch)\b",                  "NL"),
        (r"\b(pol|polish)\b",                     "PL"),
        (r"\b(swe|swedish)\b",                    "SV"),
        (r"\b(nor|norwegian)\b",                  "NO"),
        (r"\b(dan|danish)\b",                     "DA"),
        (r"\b(fin|finnish)\b",                    "FI"),
        (r"\b(tur|turkish)\b",                    "TR"),
        (r"\b(ara|arabic)\b",                     "AR"),
        (r"\b(heb|hebrew)\b",                     "HE"),
        (r"\b(tha|thai)\b",                       "TH"),
        (r"\b(vie|vietnamese)\b",                 "VI"),
        (r"\b(cze|ces|czech)\b",                  "CS"),
        (r"\b(hun|hungarian)\b",                  "HU"),
        (r"\b(ukr|ukrainian)\b",                  "UK"),
    ]

    @staticmethod
    def parse_meta(st):
        """Pull the structured bits out of an addon's free-text labels.

        Addons pack everything into `name` and `title` as multi-line text
        aimed at a browser: emoji, flags, line breaks. On a 960x544 panel
        that has to be split into fields the client can lay out and colour,
        or every row is an unreadable wall of filename.
        """
        name  = (st.get("name")  or "")
        title = (st.get("title") or "")
        blob  = name + "\n" + title
        hints = st.get("behaviorHints") or {}

        # Provider: first line of `name`, minus any parenthetical suffix.
        provider = name.split("\n")[0].strip()
        provider = re.sub(r"\s*\(.*?\)\s*", " ", provider).strip()

        # Resolution, plus HDR/DV which matter more than the extra pixels.
        res = ""
        m = re.search(r"\b(2160p|1440p|1080p|720p|576p|480p|4k|8k)\b",
                      blob, re.I)
        if m:
            res = m.group(1).lower().replace("4k", "2160p")
        extra = []
        if re.search(r"\bDV\b|dolby.?vision", blob, re.I):   extra.append("DV")
        elif re.search(r"\bHDR10\+|\bHDR\b", blob, re.I):   extra.append("HDR")
        quality = " ".join(x for x in [res] + extra if x)

        # Size: prefer the exact byte count when the addon supplies it.
        size = ""
        vs = hints.get("videoSize")
        if isinstance(vs, (int, float)) and vs > 0:
            gb = vs / (1024.0 ** 3)
            size = "%.1f GB" % gb if gb >= 1 else "%.0f MB" % (vs / 1048576.0)
        else:
            m = re.search(r"([\d.]+)\s*(GB|MB|TB)", blob, re.I)
            if m:
                size = "%s %s" % (m.group(1), m.group(2).upper())

        m = re.search(r"👤\s*(\d+)", blob)
        seeders = m.group(1) if m else ""
        if seeders in ("0", "00"):
            seeders = ""      # "0 seeds" reads as a warning; absence is fine

        # Tracker, when present, is more informative than the addon name.
        m = re.search(r"⚙️\s*([^\n👤💾]+)", blob)
        if m:
            tracker = m.group(1).strip()
            if tracker and tracker.lower() not in provider.lower():
                provider = tracker[:30]

        langs = []
        for pair in re.findall(r"[\U0001F1E6-\U0001F1FF]{2}", blob):
            code = "".join(chr(ord(c) - 0x1F1E6 + 65) for c in pair)
            lang = Handler.FLAG_LANG.get(code, code)
            if lang not in langs:
                langs.append(lang)

        # Most releases name their languages in the filename rather than as
        # flag emoji, and plenty of addons emit no flags at all -- so relying
        # on emoji alone left this empty far more often than not.
        if not langs:
            for pat, lab in Handler.LANG_TOKENS:
                if re.search(pat, blob, re.I) and lab not in langs:
                    langs.append(lab)
        if re.search(r"\bmulti\b", blob, re.I) and "MULTI" not in langs:
            langs.insert(0, "MULTI")
        if re.search(r"\bdual[.\s-]?audio\b", blob, re.I) and "DUAL" not in langs:
            langs.insert(0, "DUAL")

        # Filename: the recognisable part. Prefer the explicit hint, else the
        # first line of title, else the addon's own label.
        fname = hints.get("filename") or title.split("\n")[0] or name
        fname = " ".join(str(fname).split())

        tags = []
        for pat, lab in ((r"x265|h\.?265|hevc", "HEVC"),
                         (r"av1", "AV1"),
                         (r"atmos", "ATMOS"),
                         (r"truehd", "TRUEHD"),
                         (r"\bdts", "DTS")):
            if re.search(pat, blob, re.I):
                tags.append(lab)

        return {
            "file":     fname[:110],
            "quality":  quality or "SD",
            "size":     size,
            "seeders":  seeders,
            "provider": provider[:30] or "source",
            # Up to four; more than that is a "MULTI" release and the list
            # stops being informative.
            "langs":    "/".join(langs[:4]),
            "tags":     " ".join(tags[:3]),
        }

    # Queried in addition to whatever the account has installed, so
    # subtitles work even when no subtitle addon is configured.
    SUBTITLE_FALLBACK = "https://opensubtitles-v3.strem.io"

    ISO3 = {"eng": "EN", "spa": "ES", "fre": "FR", "fra": "FR", "ger": "DE",
            "deu": "DE", "ita": "IT", "por": "PT", "rus": "RU", "jpn": "JA",
            "kor": "KO", "chi": "ZH", "zho": "ZH", "ara": "AR", "dut": "NL",
            "nld": "NL", "pol": "PL", "swe": "SV", "nor": "NO", "dan": "DA",
            "fin": "FI", "tur": "TR", "gre": "EL", "ell": "EL", "heb": "HE",
            "hin": "HI", "tha": "TH", "vie": "VI", "cze": "CS", "ces": "CS",
            "hun": "HU", "rom": "RO", "ron": "RO", "ukr": "UK", "ind": "ID"}

    def subtracks(self, ctype, sid):
        """List available subtitle tracks. Fields: urlkey, lang, label.

        Subtitles come from the addon protocol rather than from the video
        file. Demuxing an embedded track would mean reading the whole
        source -- tens of gigabytes for a remux -- where this is a few KB.
        """
        rows, seen = [], set()
        bases = [addon_base(a) for a in ADDONS]
        if self.SUBTITLE_FALLBACK not in bases:
            bases.append(self.SUBTITLE_FALLBACK)

        for base in bases:
            url = "%s/subtitles/%s/%s.json" % (base, ctype,
                                               urllib.parse.quote(sid))
            try:
                data = fetch_json(url, timeout=20)
            except Exception:
                continue

            for sub in data.get("subtitles", []):
                surl = sub.get("url")
                if not surl or surl in seen:
                    continue
                seen.add(surl)

                raw  = (sub.get("lang") or "").lower()
                code = self.ISO3.get(raw[:3], raw[:2].upper() or "??")
                rows.append([
                    base64.urlsafe_b64encode(surl.encode()).decode().rstrip("="),
                    code,
                    (sub.get("id") or raw or "subtitle")[:40],
                ])

        # Group by language so picking one of several English options is a
        # matter of trying the next line, not hunting through the list.
        rows.sort(key=lambda r: r[1])
        self.log_message("subtitles for %s -> %d tracks", sid, len(rows))
        return pack(rows[:40])

    def subs(self, urlkey):
        """Fetch one subtitle file and flatten it to timed cues.

        Fields: start_ms, end_ms, text. Parsing on the server keeps the Vita
        free of format handling, and the cue list for a whole film is only
        tens of kilobytes.
        """
        pad = "=" * (-len(urlkey) % 4)
        url = base64.urlsafe_b64decode(urlkey + pad).decode()

        req = urllib.request.Request(url, headers={
            "User-Agent": "Mozilla/5.0 (compatible; vitastremio/1)"})
        with urllib.request.urlopen(req, timeout=25) as r:
            raw = r.read(4 * 1024 * 1024)

        text = raw.decode("utf-8", "replace")
        if text[:1] == "\ufeff":
            text = text[1:]

        cues = []
        # Matches both SRT (00:00:01,000) and WebVTT (00:00:01.000).
        pat = re.compile(
            r"(\d{1,2}):(\d{2}):(\d{2})[,.](\d{1,3})\s*-->\s*"
            r"(\d{1,2}):(\d{2}):(\d{2})[,.](\d{1,3})")

        blocks = re.split(r"\r?\n\r?\n", text)
        for blk in blocks:
            m = pat.search(blk)
            if not m:
                continue
            g = [int(x) for x in m.groups()]
            start = (g[0]*3600 + g[1]*60 + g[2]) * 1000 + g[3]
            end   = (g[4]*3600 + g[5]*60 + g[6]) * 1000 + g[7]

            body = blk[m.end():].strip()
            body = re.sub(r"<[^>]+>", "", body)          # html-ish tags
            body = re.sub(r"\{\\[^}]*\}", "", body)       # ASS overrides
            body = " | ".join(l.strip() for l in body.splitlines() if l.strip())
            if not body:
                continue
            cues.append([str(start), str(end), body[:150]])

        self.log_message("subtitle %s -> %d cues", url[:60], len(cues))
        return pack(cues[:4000])

    def streams(self, ctype, sid):
        """Fields: key, release, quality, size, seeds, provider, langs, tags

        Eight fields is the wire format's maximum and exactly what a useful
        row needs. The client lays them out; sending one pre-formatted string
        would force every layout decision onto the server.

        Addon ordering is preserved deliberately. Aggregators sort by the
        user's own configured preferences (cached first, quality, language),
        and re-sorting here would silently override that config.
        """
        rows = []
        for man in ADDONS:
            base = addon_base(man)
            url = "%s/stream/%s/%s.json" % (base, ctype,
                                            urllib.parse.quote(sid))
            try:
                data = fetch_json(url, timeout=TIMEOUT_STREAM)
            except Exception as e:
                self.log_message("stream lookup failed (%s): %s",
                                 base.split("/")[2], e)
                continue

            for s in data.get("streams", []):
                # Addons advertise non-playable entries: catalog links, "no
                # results" placeholders, external-player handoffs. They carry
                # neither a url nor an infoHash, so key_decode would build a
                # garbage path and the Vita would sit on a black screen with
                # nothing in the log. Drop them here instead.
                if not s.get("url") and not s.get("infoHash"):
                    continue

                d = self.parse_stream(s)
                rows.append([
                    key_encode(s),
                    d["release"][:110],
                    d["quality"],
                    d["size"],
                    ("C" if d["cached"] else "") + d["seeds"],
                    d["provider"][:20],
                    d["langs"],
                    d["tags"],
                ])

        if len(rows) > MAX_STREAMS:
            rows = rows[:MAX_STREAMS]

        self.log_message("streams %s -> %d", sid, len(rows))
        return pack(rows)


def vaapi_probe():
    """Confirm the GPU encoder actually works.

    Must go through format=nv12,hwupload: h264_vaapi cannot accept frames
    that live in system memory, so a probe without the upload step fails even
    on a perfectly healthy machine and produces a false alarm.
    """
    cmd = [
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin",
        "-vaapi_device", RENDER_NODE,
        "-f", "lavfi", "-i", "testsrc=duration=1:size=320x240:rate=30",
        "-vf", "format=nv12,hwupload",
        "-c:v", "h264_vaapi", "-f", "null", "-",
    ]
    try:
        r = subprocess.run(cmd, capture_output=True, timeout=25)
    except Exception as e:
        return False, str(e)
    if r.returncode == 0:
        return True, ""
    return False, r.stderr.decode("utf-8", "replace").strip()[-400:]


VAAPI_OK = None
VAAPI_WHY = ""



# Same idea for the addon collection: if the response shape moves again,
# show what came back rather than reporting a bare "no addons".
LAST_ADDON_SHAPE = ""


def set_addon_shape(text):
    global LAST_ADDON_SHAPE
    LAST_ADDON_SHAPE = text


def stremio_reachable():
    try:
        req = urllib.request.Request(STREMIO_URL + "/settings",
                                     headers={"User-Agent": "vitastremio/1"})
        with urllib.request.urlopen(req, timeout=1.5):
            return True
    except Exception:
        return False


def main():
    global ADDONS, VAAPI_OK, VAAPI_WHY

    if "--auth-key" in sys.argv:
        # Escape hatch: paste a key pulled from the Stremio web app instead
        # of logging in through the API. In the browser on app.strem.io, open
        # devtools -> Application -> Local Storage and copy the authKey value.
        i = sys.argv.index("--auth-key")
        if i + 1 >= len(sys.argv):
            print("usage: middleware.py --auth-key YOUR_KEY")
            return 1
        key = sys.argv[i + 1].strip().strip('"\'')
        with open(AUTH_PATH, "w") as f:
            f.write(key)
        os.chmod(AUTH_PATH, 0o600)
        print("auth key saved to %s (mode 600)" % AUTH_PATH)
        try:
            addons = fetch_account_addons(key)
            print("account has %d addons:" % len(addons))
            for a in addons:
                print("   ", a[:88])
        except Exception as e:
            print("key saved, but fetching addons failed: %s" % e)
            return 1
        return 0

    ADDONS = resolve_addons()

    ok, why = vaapi_probe()
    VAAPI_OK, VAAPI_WHY = ok, why
    if ok:
        print("VAAPI encode OK on %s" % RENDER_NODE)
    else:
        print("!! VAAPI encode FAILED on %s -- transcodes will fall back to\n"
              "!! software and peg the CPU. Check `vainfo` and access to\n"
              "!! /dev/dri. ffmpeg said:\n%s" % (RENDER_NODE, why))

    if load_auth_key():
        sync_from_account()
        threading.Thread(target=sync_loop, daemon=True).start()
        print("[mw] account sync every %d min" % max(1, SYNC_INTERVAL // 60))

    srv = ThreadingHTTPServer(("0.0.0.0", LISTEN_PORT), Handler)
    srv.daemon_threads = True
    print("vitastremio middleware on :%d -> %s" % (LISTEN_PORT, STREMIO))
    print("configure addons at http://<this-host>:%d/" % LISTEN_PORT)
    for a in ADDONS:
        print("  addon: %s" % a[:96])
    srv.serve_forever()


if __name__ == "__main__":
    sys.exit(main() or 0)
