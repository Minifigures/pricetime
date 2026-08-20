// Where the narrative stage gets its model.
//
// The engine has exactly zero runtime dependencies and this keeps that
// promise: every provider below is a plain fetch against an HTTPS endpoint,
// no SDK. Adding one is a table entry, not a code path.
//
// The list is ordered. The route picks the first provider whose key is
// present in the environment, so which model writes the case note is a
// deployment decision rather than a code change, and losing a free tier means
// setting a different variable rather than editing this file.
//
// None of this touches the engine. The detectors are deterministic C++ and
// their findings are computed before any of this runs.

export interface Provider {
  readonly name: string;
  readonly envKey: string;
  /** Build the request. `key` is the value of envKey. */
  readonly request: (
    key: string,
    system: string,
    user: string,
  ) => { url: string; init: RequestInit };
  /**
   * Pull the incremental text out of one parsed SSE `data:` payload.
   * Return null for frames that carry no text (pings, role headers, usage).
   */
  readonly delta: (frame: unknown) => string | null;
}

/** Most of these APIs are OpenAI-compatible, so the body and delta are shared. */
function openaiish(
  name: string,
  envKey: string,
  url: string,
  model: string,
  extraHeaders: Readonly<Record<string, string>> = {},
): Provider {
  return {
    name,
    envKey,
    request: (key, system, user) => ({
      url,
      init: {
        method: "POST",
        headers: {
          "content-type": "application/json",
          authorization: `Bearer ${key}`,
          ...extraHeaders,
        },
        body: JSON.stringify({
          model,
          stream: true,
          max_tokens: 700,
          temperature: 0.3,
          messages: [
            { role: "system", content: system },
            { role: "user", content: user },
          ],
        }),
      },
    }),
    delta: (frame) => {
      const f = frame as { choices?: ReadonlyArray<{ delta?: { content?: string } }> };
      return f.choices?.[0]?.delta?.content ?? null;
    },
  };
}

export const PROVIDERS: readonly Provider[] = [
  // Anthropic. Not free, kept first only because it is what the project was
  // written against; if the key is absent the list falls through.
  {
    name: "Anthropic",
    envKey: "ANTHROPIC_API_KEY",
    request: (key, system, user) => ({
      url: "https://api.anthropic.com/v1/messages",
      init: {
        method: "POST",
        headers: {
          "content-type": "application/json",
          "x-api-key": key,
          "anthropic-version": "2023-06-01",
        },
        body: JSON.stringify({
          model: "claude-sonnet-5",
          max_tokens: 700,
          system,
          stream: true,
          messages: [{ role: "user", content: user }],
        }),
      },
    }),
    delta: (frame) => {
      const f = frame as { type?: string; delta?: { text?: string } };
      return f.type === "content_block_delta" ? (f.delta?.text ?? null) : null;
    },
  },
];

/** The first provider whose key is set, or null if none are. */
export function pick(env: Record<string, string | undefined>): {
  provider: Provider;
  key: string;
} | null {
  for (const p of PROVIDERS) {
    const key = env[p.envKey];
    if (key !== undefined && key !== "") return { provider: p, key };
  }
  return null;
}
