#!/usr/bin/env bash
# Records real Messages API SSE streams as self-test fixtures (Task 2).
# The key comes from the keyring and reaches curl on STDIN (-H @-), never argv;
# every fixture is checked for message_stop and for NOT containing the key.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/fixtures/sse"
mkdir -p "$OUT"
KEY="$(secret-tool lookup service anthropic account default)" || { echo "FAIL: no test key in the keyring"; exit 1; }
[ -n "$KEY" ] || { echo "FAIL: empty test key"; exit 1; }
post() { # $1 = output file, $2 = JSON body
   printf 'x-api-key: %s\nanthropic-version: 2023-06-01\ncontent-type: application/json\n' "$KEY" \
      | curl -sSN --fail-with-body -H @- --data-binary "$2" https://api.anthropic.com/v1/messages > "$1"
}
TOOL='{"name":"describe_process","description":"Describe a PixInsight process by id.","input_schema":{"type":"object","properties":{"id":{"type":"string"}},"required":["id"]}}'
post "$OUT/text-opus-4-8.sse" \
   '{"model":"claude-opus-4-8","max_tokens":200,"stream":true,"messages":[{"role":"user","content":"Reply with exactly: café ok — done"}]}'
post "$OUT/tool-opus-4-8.sse" \
   '{"model":"claude-opus-4-8","max_tokens":400,"stream":true,"tools":['"$TOOL"'],"messages":[{"role":"user","content":"Call describe_process for PixelMath."}]}'
post "$OUT/thinking-tool-opus-5-5.sse" \
   '{"model":"claude-opus-5-5","max_tokens":4000,"stream":true,"tools":['"$TOOL"'],"messages":[{"role":"user","content":"Think about which PixInsight process removes a green colour cast, then call describe_process on it."}]}'
for f in "$OUT"/*.sse; do
   grep -q '^event: message_stop' "$f" || { echo "FAIL: $f has no message_stop"; exit 1; }
   if grep -qF -- "$KEY" "$f"; then echo "FAIL: the key appears in $f"; rm -f "$f"; exit 1; fi
   echo "ok: $f ($(wc -c < "$f") bytes)"
done
