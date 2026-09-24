#!/usr/bin/env bash
# Query the PCL or PJSR language server (MCP over stdio) from the shell.
# Usage: tools/pcl-query.sh <tool> '<json-args>'
#   tools/pcl-query.sh pcl_class_info '{"className":"ProcessInstance"}'
#   tools/pcl-query.sh pjsr_class_info '{"className":"ImageWindow"}'
# Tools starting with "pjsr_" go to tools/pjsr_parser, everything else to tools/pcl_parser.
set -euo pipefail
tool="${1:?tool name}"; args="${2:-{\}}"
here="$(cd "$(dirname "$0")" && pwd)"
case "$tool" in pjsr_*) dir="$here/pjsr_parser" ;; *) dir="$here/pcl_parser" ;; esac
req=$(python3 -c 'import json,sys; print(json.dumps({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":sys.argv[1],"arguments":json.loads(sys.argv[2])}}))' "$tool" "$args")
{ printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"pcl-query","version":"1"}}}' "$req"; sleep 1; } \
  | (cd "$dir" && timeout 15 node src/mcp-server.js 2>/dev/null) \
  | python3 -c '
import json,sys
for line in sys.stdin:
    line=line.strip()
    if not line: continue
    m=json.loads(line)
    if m.get("id")!=2: continue
    if "error" in m: print("ERROR:", m["error"]); sys.exit(1)
    for c in m["result"].get("content",[]): print(c.get("text",""))
'
