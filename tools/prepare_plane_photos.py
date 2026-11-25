#!/usr/bin/env python3
"""Download representative aircraft photos for SkyFrame's offline plane library.

When you tap a plane, SkyFrame shows <card>/planes/types/<TYPE>.jpg straight away
(TYPE is the ICAO type designator from ADS-B, e.g. B738) while it downloads a photo
of that exact airframe. This script fills that folder: for each common type it takes
the lead image of the type's English Wikipedia article (hosted on Wikimedia Commons),
crops it to the details-page frame (280x156) and saves a small baseline JPEG.
Photo credits are written to ATTRIBUTION.txt next to the photos.

SkyFrame also lists every type it sees in traffic without a photo in planes/missing.txt
("TYPE<tab>description", e.g. "C172<tab>CESSNA 172 Skyhawk"). --missing fetches those:
known types from the list below, the rest by searching Wikipedia for the description;
types found are taken off the list.

Requires Pillow:  python -m pip install pillow
Usage:            python tools/prepare_plane_photos.py F:/planes      (F: = the microSD card)
                  python tools/prepare_plane_photos.py F:/planes --only B738,A320 --force
                  python tools/prepare_plane_photos.py F:/planes --missing
"""
import argparse
import io
import json
import sys
import time
import urllib.parse
import urllib.request
from pathlib import Path

from PIL import Image, ImageOps

PHOTO_SIZE = (280, 156)   # must match SKYFRAME_PHOTO_W x SKYFRAME_PHOTO_H in the firmware
USER_AGENT = "SkyFrame-plane-photos/1.0 (https://github.com/CosmicYeet/SkyFrame; personal use)"
API = "https://en.wikipedia.org/w/api.php"

# ICAO type designator -> English Wikipedia article. Several designators can share an article.
TYPES = {
    # Boeing airliners
    "B712": "Boeing 717",
    "B733": "Boeing 737 Classic", "B734": "Boeing 737 Classic", "B735": "Boeing 737 Classic",
    "B736": "Boeing 737 Next Generation", "B737": "Boeing 737 Next Generation",
    "B738": "Boeing 737 Next Generation", "B739": "Boeing 737 Next Generation",
    "B37M": "Boeing 737 MAX", "B38M": "Boeing 737 MAX", "B39M": "Boeing 737 MAX", "B3XM": "Boeing 737 MAX",
    "B752": "Boeing 757", "B753": "Boeing 757",
    "B762": "Boeing 767", "B763": "Boeing 767", "B764": "Boeing 767",
    "B772": "Boeing 777", "B773": "Boeing 777", "B77L": "Boeing 777", "B77W": "Boeing 777",
    "B788": "Boeing 787 Dreamliner", "B789": "Boeing 787 Dreamliner", "B78X": "Boeing 787 Dreamliner",
    "B744": "Boeing 747-400", "B748": "Boeing 747-8",
    "MD11": "McDonnell Douglas MD-11", "MD88": "McDonnell Douglas MD-80", "MD90": "McDonnell Douglas MD-90",
    # Airbus airliners
    "A318": "Airbus A320 family", "A319": "Airbus A320 family",
    "A320": "Airbus A320 family", "A321": "Airbus A320 family",
    "A19N": "Airbus A320neo family", "A20N": "Airbus A320neo family", "A21N": "Airbus A320neo family",
    "BCS1": "Airbus A220", "BCS3": "Airbus A220",
    "A306": "Airbus A300", "A332": "Airbus A330", "A333": "Airbus A330",
    "A338": "Airbus A330neo", "A339": "Airbus A330neo",
    "A359": "Airbus A350", "A35K": "Airbus A350", "A388": "Airbus A380",
    # Regional
    "E170": "Embraer E-Jet family", "E75L": "Embraer E-Jet family", "E75S": "Embraer E-Jet family",
    "E190": "Embraer E-Jet family", "E195": "Embraer E-Jet family",
    "E290": "Embraer E-Jet E2 family", "E295": "Embraer E-Jet E2 family",
    "E135": "Embraer ERJ family", "E145": "Embraer ERJ family", "E45X": "Embraer ERJ family",
    "CRJ2": "Bombardier CRJ100/200", "CRJ7": "Bombardier CRJ700 series",
    "CRJ9": "Bombardier CRJ700 series", "CRJX": "Bombardier CRJ700 series",
    "DH8A": "De Havilland Canada Dash 8", "DH8B": "De Havilland Canada Dash 8",
    "DH8C": "De Havilland Canada Dash 8", "DH8D": "De Havilland Canada Dash 8",
    "AT43": "ATR 42", "AT45": "ATR 42", "AT72": "ATR 72", "AT75": "ATR 72", "AT76": "ATR 72",
    "SB20": "Saab 2000", "SF34": "Saab 340",
    # Business jets and turboprops
    "C25A": "Cessna CitationJet/M2", "C25B": "Cessna CitationJet/M2", "C25C": "Cessna CitationJet/M2",
    "C510": "Cessna Citation Mustang", "C525": "Cessna CitationJet/M2",
    "C550": "Cessna Citation II", "C560": "Cessna Citation V", "C56X": "Cessna Citation Excel",
    "C650": "Cessna Citation III", "C680": "Cessna Citation Sovereign", "C68A": "Cessna Citation Latitude",
    "C700": "Cessna Citation Longitude", "C750": "Cessna Citation X",
    "CL30": "Bombardier Challenger 300", "CL35": "Bombardier Challenger 300",
    "CL60": "Bombardier Challenger 600 series",
    "GLEX": "Bombardier Global Express", "GL5T": "Bombardier Global Express", "GL7T": "Bombardier Global 7500",
    "GLF4": "Gulfstream IV", "GLF5": "Gulfstream V", "GLF6": "Gulfstream G650", "GA6C": "Gulfstream G600",
    "G280": "Gulfstream G280", "G150": "Gulfstream G150",
    "LJ35": "Learjet 35", "LJ45": "Learjet 45", "LJ60": "Learjet 60", "LJ75": "Learjet 45",
    "E50P": "Embraer Phenom 100", "E55P": "Embraer Phenom 300", "E545": "Embraer Legacy 500",
    "E550": "Embraer Legacy 500", "E35L": "Embraer Legacy 600",
    "F2TH": "Dassault Falcon 2000", "F900": "Dassault Falcon 900", "FA7X": "Dassault Falcon 7X",
    "FA8X": "Dassault Falcon 8X", "FA50": "Dassault Falcon 50",
    "HDJT": "Honda HA-420 HondaJet", "SF50": "Cirrus Vision SF50",
    "PC12": "Pilatus PC-12", "PC24": "Pilatus PC-24",
    "BE20": "Beechcraft Super King Air", "B350": "Beechcraft Super King Air",
    "BE9L": "Beechcraft King Air", "BE40": "Beechcraft Beechjet",
    "TBM7": "Socata TBM", "TBM8": "Socata TBM", "TBM9": "Socata TBM",
    "C208": "Cessna 208 Caravan", "P180": "Piaggio P.180 Avanti",
    # Light aircraft
    "C150": "Cessna 150", "C152": "Cessna 152", "C172": "Cessna 172", "C182": "Cessna 182 Skylane",
    "C206": "Cessna 206", "C210": "Cessna 210 Centurion", "C310": "Cessna 310", "C340": "Cessna 340",
    "SR20": "Cirrus SR20", "SR22": "Cirrus SR22", "S22T": "Cirrus SR22",
    "P28A": "Piper PA-28 Cherokee", "P28B": "Piper PA-28 Cherokee", "P28R": "Piper PA-28R Cherokee Arrow",
    "PA32": "Piper PA-32", "PA34": "Piper PA-34 Seneca", "PA44": "Piper PA-44 Seminole",
    "PA46": "Piper PA-46", "PA31": "Piper PA-31 Navajo", "PA18": "Piper PA-18 Super Cub",
    "BE35": "Beechcraft Bonanza", "BE36": "Beechcraft Bonanza", "BE58": "Beechcraft Baron",
    "DA40": "Diamond DA40 Diamond Star", "DA42": "Diamond DA42 Twin Star", "DA62": "Diamond DA62",
    "M20P": "Mooney M20", "M20T": "Mooney M20", "RV7": "Van's Aircraft RV-7", "RV8": "Van's Aircraft RV-8",
    # Helicopters
    "R22": "Robinson R22", "R44": "Robinson R44", "R66": "Robinson R66",
    "EC35": "Eurocopter EC135", "EC45": "Eurocopter EC145", "EC30": "Eurocopter EC130",
    "AS50": "Eurocopter AS350 Écureuil", "AS65": "Eurocopter AS365 Dauphin",
    "B06": "Bell 206", "B407": "Bell 407", "B429": "Bell 429 GlobalRanger", "B412": "Bell 412",
    "S76": "Sikorsky S-76", "S92": "Sikorsky S-92", "H60": "Sikorsky UH-60 Black Hawk",
    "A109": "AgustaWestland AW109", "A139": "AgustaWestland AW139", "A169": "AgustaWestland AW169",
    "V22": "Bell Boeing V-22 Osprey",
    # Military and government
    "C130": "Lockheed C-130 Hercules", "C30J": "Lockheed Martin C-130J Super Hercules",
    "C17": "Boeing C-17 Globemaster III", "C5M": "Lockheed C-5 Galaxy",
    "K35R": "Boeing KC-135 Stratotanker", "KC46": "Boeing KC-46 Pegasus", "P8": "Boeing P-8 Poseidon",
    "E3TF": "Boeing E-3 Sentry", "C2": "Grumman C-2 Greyhound", "E2": "Northrop Grumman E-2 Hawkeye",
    "F16": "General Dynamics F-16 Fighting Falcon", "F15": "McDonnell Douglas F-15 Eagle",
    "F18S": "Boeing F/A-18E/F Super Hornet", "F35": "Lockheed Martin F-35 Lightning II",
    "A10": "Fairchild Republic A-10 Thunderbolt II", "T38": "Northrop T-38 Talon", "T6": "Beechcraft T-6 Texan II",
    "U2": "Lockheed U-2", "VC25": "Boeing VC-25",
}


def get_json(params):
    url = API + "?" + urllib.parse.urlencode(params)
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.load(r)


def lead_images(titles):
    """title -> (thumbnail URL, Commons file name, article URL) for each article that has a lead image."""
    found = {}
    titles = sorted(set(titles))
    for i in range(0, len(titles), 50):           # API limit: 50 titles per request
        batch = titles[i:i + 50]
        data = get_json({
            "action": "query", "format": "json", "formatversion": "2", "redirects": "1",
            "prop": "pageimages|info", "inprop": "url", "piprop": "thumbnail|name", "pithumbsize": "800",
            "titles": "|".join(batch),
        })
        q = data.get("query", {})
        # Follow normalisation + redirects back to the title we asked for.
        alias = {}
        for step in q.get("normalized", []) + q.get("redirects", []):
            alias[step["from"]] = step["to"]
        def final(t):
            seen = set()
            while t in alias and t not in seen:
                seen.add(t)
                t = alias[t]
            return t
        pages = {p["title"]: p for p in q.get("pages", [])}
        for t in batch:
            p = pages.get(final(t))
            if p and "thumbnail" in p and "pageimage" in p:
                found[t] = (p["thumbnail"]["source"], p["pageimage"], p.get("fullurl", ""))
    return found


def tidy_description(desc):
    """'BOEING 737-800' -> 'Boeing 737-800', 'CESSNA 172 Skyhawk' -> 'Cessna 172 Skyhawk'."""
    words = []
    for w in desc.split():
        words.append(w.capitalize() if w.isalpha() and w.isupper() and len(w) > 3 else w)
    return " ".join(words)


def search_article(desc):
    """The English Wikipedia article that best matches an aircraft description, or None."""
    data = get_json({"action": "query", "format": "json", "formatversion": "2", "list": "search",
                     "srsearch": tidy_description(desc) + " aircraft", "srlimit": "1", "srnamespace": "0"})
    hits = data.get("query", {}).get("search", [])
    return hits[0]["title"] if hits else None


def read_missing(folder):
    """type -> description from SkyFrame's planes/missing.txt."""
    path = folder / "missing.txt"
    found = {}
    if path.exists():
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            code, _, desc = line.partition("\t")
            code = code.strip().upper()
            if code and code.isalnum():
                found[code] = desc.strip()
    return found


def read_attribution(folder):
    """code -> (title, Commons file name, article URL) already in ATTRIBUTION.txt."""
    path = folder / "ATTRIBUTION.txt"
    known = {}
    if path.exists():
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            parts = line.split("\t")
            if len(parts) == 3 and parts[2].startswith("https://commons.wikimedia.org/wiki/File:"):
                name = urllib.parse.unquote(parts[2].rsplit("File:", 1)[1]).replace("_", " ")
                known[parts[0]] = (parts[1], name, "")
    return known


def download(url):
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read()


def save_photo(data, path):
    with Image.open(io.BytesIO(data)) as im:
        im = ImageOps.exif_transpose(im).convert("RGB")
        im = ImageOps.fit(im, PHOTO_SIZE, method=Image.Resampling.LANCZOS, centering=(0.5, 0.5))
        # Baseline (not progressive): the board's decoder only handles baseline JPEG.
        im.save(path, format="JPEG", quality=88, optimize=True, progressive=False)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("destination", type=Path, help="the card's planes folder, e.g. F:/planes")
    ap.add_argument("--only", help="comma-separated type codes to fetch (default: all)")
    ap.add_argument("--force", action="store_true", help="replace photos that already exist")
    ap.add_argument("--missing", action="store_true",
                    help="fetch the types SkyFrame listed in missing.txt (searching Wikipedia for unknown ones)")
    a = ap.parse_args()

    wanted = TYPES
    missing = {}
    if a.missing:
        missing = read_missing(a.destination)
        if not missing:
            print("missing.txt lists no types: nothing to fetch.")
            return 0
        wanted = {}
        print(f"{len(missing)} type(s) listed in missing.txt:")
        for code, desc in sorted(missing.items()):
            title = TYPES.get(code) or (search_article(desc) if desc else None)
            print(f"  {code:5} {desc or '(no description)'} -> {title or 'no article found'}")
            if title:
                wanted[code] = title
            time.sleep(0.2)
    elif a.only:
        codes = [c.strip().upper() for c in a.only.split(",") if c.strip()]
        unknown = [c for c in codes if c not in TYPES]
        if unknown:
            ap.error("unknown type code(s): " + ", ".join(unknown))
        wanted = {c: TYPES[c] for c in codes}

    out = a.destination / "types"
    out.mkdir(parents=True, exist_ok=True)
    print(f"Looking up {len(set(wanted.values()))} Wikipedia articles...")
    images = lead_images(wanted.values())

    credits, cache, done, skipped, failed = read_attribution(a.destination), {}, 0, 0, []
    for code, title in sorted(wanted.items()):
        path = out / f"{code}.jpg"
        if title not in images:
            failed.append(f"{code} ({title}: no lead image)")
            continue
        thumb, file_name, page = images[title]
        credits[code] = (title, file_name, page)
        if path.exists() and not a.force:
            skipped += 1
            continue
        try:
            if thumb not in cache:
                cache[thumb] = download(thumb)
                time.sleep(0.2)                     # be polite to Wikimedia
            save_photo(cache[thumb], path)
            done += 1
            print(f"  {code:5} {title}")
        except Exception as e:                      # keep going; report at the end
            failed.append(f"{code} ({title}: {e})")

    with open(a.destination / "ATTRIBUTION.txt", "w", encoding="utf-8") as f:
        f.write("SkyFrame representative aircraft photos (types/<TYPE>.jpg)\n"
                "Lead images of English Wikipedia articles, hosted on Wikimedia Commons.\n"
                "Each photo's author and license are on its Commons file page below.\n\n")
        for code, (title, file_name, page) in sorted(credits.items()):
            commons = "https://commons.wikimedia.org/wiki/File:" + urllib.parse.quote(file_name.replace(" ", "_"))
            f.write(f"{code}\t{title}\t{commons}\n")

    if a.missing:   # what's on the card now comes off the list; the rest stays for next time
        left = {c: d for c, d in missing.items() if not (out / f"{c}.jpg").exists()}
        with open(a.destination / "missing.txt", "w", encoding="utf-8", newline="\n") as f:
            for code, desc in sorted(left.items()):
                f.write(f"{code}\t{desc}\n")
        print(f"missing.txt: {len(missing) - len(left)} found, {len(left)} still without a photo")

    print(f"\nSaved {done} photo(s), kept {skipped} existing, {len(failed)} failed -> {out}")
    for item in failed:
        print("  failed:", item)
    return 1 if failed and not done and not skipped else 0


if __name__ == "__main__":
    sys.exit(main())
