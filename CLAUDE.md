Plain C11 + vendored cJSON; no other dependencies. Keep it small. Never touch `~/.diotima`
in tests (use a temp `DIOTIMA_HOME`). Phase and position are always derived from the
transcript — never add a stored progress field. Tools are contracts: reject bad calls back
to the model with an agent-readable error. Licensed AGPL-3.0 (prompts come from learnharness).
