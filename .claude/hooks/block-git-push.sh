#!/usr/bin/env bash
INPUT=$(cat)

if command -v jq >/dev/null 2>&1; then
  CMD=$(printf '%s' "$INPUT" | jq -r '.tool_input.command // empty')
else
  CMD=$(printf '%s' "$INPUT" | python3 -c 'import sys, json; print(json.load(sys.stdin).get("tool_input", {}).get("command", ""))' 2>/dev/null)
fi

if echo "$CMD" | grep -Eq '(\bgit\b.*\b(commit|push)\b|\.claude(/hooks|/settings\.json)?)'; then
  cat << 'JSON'
{
  "hookSpecificOutput": {
    "hookEventName": "PreToolUse",
    "permissionDecision": "deny",
    "permissionDecisionReason": "Direct git commit, git push, and modifying .claude/ are blocked by repo policy."
  }
}
JSON
  exit 0
fi
