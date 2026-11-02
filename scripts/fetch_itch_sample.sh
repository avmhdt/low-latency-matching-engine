#!/usr/bin/env bash
# Nasdaq ITCH 5.0 sample files.
#   scripts/fetch_itch_sample.sh --list        print the file names on the listing page
#   scripts/fetch_itch_sample.sh <filename>    download one named file into data/raw/itch/
# Never downloads without an explicit file name; a repeat run resumes with curl -C -.
set -euo pipefail

base_url="https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH"
listing_url="$base_url/"

usage() {
  sed -n '2,4p' "$0" | sed 's/^# \{0,1\}//'
}

if [ "$#" -ne 1 ]; then
  usage >&2
  exit 2
fi

case "$1" in
  -h | --help)
    usage
    ;;
  --list)
    # Quiet on stdout apart from names, so a blocked page shows up as an empty list and exit 1.
    names=$(curl -fsSL --max-time 60 -A "Mozilla/5.0" "$listing_url" |
      grep -oE '[0-9]{8}\.[A-Z_]*ITCH[0-9_]*\.gz' | sort -u)
    if [ -z "$names" ]; then
      echo "no file names found on $listing_url" >&2
      exit 1
    fi
    printf '%s\n' "$names"
    ;;
  -*)
    usage >&2
    exit 2
    ;;
  *)
    dest_dir="${LLME_DATA_DIR:-data}/raw/itch"
    dest="$dest_dir/$1"
    mkdir -p "$dest_dir"
    curl -fL -C - --retry 5 -A "Mozilla/5.0" -o "$dest" "$base_url/$1"
    ls -l "$dest"
    sha256sum "$dest"
    ;;
esac
