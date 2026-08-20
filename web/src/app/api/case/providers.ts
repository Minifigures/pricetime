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
  // Free tiers first, deliberately. This runs on a public demo and the cost of
  // a judge clicking the button should be zero.
  //
  // OpenRouter's :free variants need no card. The endpoint for this model
  // reports is_moderated false, so there is no extra moderation layer over a
  // prompt that necessarily uses words like spoofing and layering in their
  // regulatory sense. reasoning.exclude keeps a reasoning model's scratchpad
  // out of a compliance document.
  {
    name: "OpenRouter (GLM 5.2, free)",
    envKey: "OPENROUTER_API_KEY",
    request: (key, system, user) => ({
      url: "https://openrouter.ai/api/v1/chat/completions",
      init: {
        method: "POST",
        headers: {
          "content-type": "application/json",
          authorization: `Bearer ${key}`,
          "HTTP-Referer": "https://pricetime-mu.vercel.app",
          "X-Title": "pricetime",
        },
        body: JSON.stringify({
          model: "z-ai/glm-5.2:free",
          stream: true,
          temperature: 0.3,
          reasoning: { exclude: true },
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
  },

  // Zhipu direct. GLM-4.7-Flash is priced at zero rather than being trial
  // credit, so it does not run out. Different vendor, so it survives an
  // OpenRouter outage. thinking disabled for the same reason as above.
  {
    name: "Z.ai (GLM 4.7 Flash, free)",
    envKey: "ZAI_API_KEY",
    request: (key, system, user) => ({
      url: "https://api.z.ai/api/paas/v4/chat/completions",
      init: {
        method: "POST",
        headers: {
          "content-type": "application/json",
          authorization: `Bearer ${key}`,
          "accept-language": "en-US,en",
        },
        body: JSON.stringify({
          model: "glm-4.7-flash",
          stream: true,
          temperature: 0.3,
          max_tokens: 900,
          thinking: { type: "disabled" },
          messages: [
            { role: "system", content: system },
            { role: "user", content: user },
          ],
        }),
      },
    }),
    delta: (frame) => {
      // Only content. A reasoning model may also emit reasoning_content, and
      // that is a scratchpad, not part of the case note.
      const f = frame as {
        choices?: ReadonlyArray<{ delta?: { content?: string } }>;
      };
      return f.choices?.[0]?.delta?.content ?? null;
    },
  },

  // Hugging Face's router, kept as a third vendor. Its free allowance is about
  // ten cents a month, which is enough to answer a judge and not enough to
  // develop against, so it sits below the two that are actually free.
  openaiish(
    "Hugging Face router",
    "HF_TOKEN",
    "https://router.huggingface.co/v1/chat/completions",
    "openai/gpt-oss-120b:cheapest",
  ),

  // Anthropic. Last because it is the only one here that costs money, so it
  // is used only when nothing free is configured.
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
