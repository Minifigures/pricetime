#!/usr/bin/env bash
# Optional narrative stage over surveillance findings.
#
#   ./scripts/explain.sh surveillance.json
#
# This is DELIBERATELY a separate process from the engine.
#
# The matching engine is deterministic, auditable, and must produce the same
# output for the same input every time. A language model is none of those
# things, so it is not allowed anywhere near a matching decision, a price, or a
# fill. What it does here is turn structured findings that the engine already
# computed into prose a compliance reviewer can read.
#
# Every number in the narrative comes from the JSON. The model is told
# explicitly that it may not invent figures, and the deterministic report
# printed by the engine remains the source of truth. If no API key is present,
# the engine's own report is the whole deliverable and nothing is lost.
set -euo pipefail

IN="${1:-surveillance.json}"
[[ -f "$IN" ]] || { echo "no findings file: $IN" >&2; exit 1; }

if [[ -z "${ANTHROPIC_API_KEY:-}" ]]; then
  cat <<'MSG'
ANTHROPIC_API_KEY is not set, so the narrative stage is skipped.

This is a supported state, not a failure. The engine already printed a complete
deterministic surveillance report; this stage only rewrites those same findings
as prose. Set the key to enable it:

  export ANTHROPIC_API_KEY=sk-ant-...
MSG
  exit 0
fi

FINDINGS=$(cat "$IN")

PROMPT=$(cat <<PROMPT_EOF
You are a market surveillance analyst. Below are findings produced by a limit
order book matching engine replaying real exchange data.

Write a short review note (under 200 words) for a compliance reviewer.

Hard rules:
- Use ONLY numbers present in the JSON. Invent nothing.
- These are screening signals. Do not assert intent, manipulation, or
  wrongdoing by any participant.
- If a finding has a regulatory basis listed, reference it plainly.
- If there are no findings, say so in one sentence and stop.

FINDINGS:
$FINDINGS
PROMPT_EOF
)

jq -n --arg p "$PROMPT" '{
  model: "claude-sonnet-4-6",
  max_tokens: 700,
  messages: [{role: "user", content: $p}]
}' > /tmp/pricetime_req.json

curl -sS https://api.anthropic.com/v1/messages \
  -H "x-api-key: $ANTHROPIC_API_KEY" \
  -H "anthropic-version: 2023-06-01" \
  -H "content-type: application/json" \
  -d @/tmp/pricetime_req.json \
| jq -r '.content[0].text // ("error: " + (.error.message // "unknown"))'
