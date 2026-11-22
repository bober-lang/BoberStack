#!/bin/sh
# Fetches tidwall/btree.c at the commit below: btree.c and LICENSE, unmodified.
set -eu
commit=48634e841e32c7c1e91d5319002bab4f5f8842f0
cd "$(dirname "$0")"
for f in btree.c LICENSE; do
	curl -fsS -A curl -o "$f" "https://raw.githubusercontent.com/tidwall/btree.c/$commit/$f"
done
