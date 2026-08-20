// The narrative stage over surveillance findings.
//
// This is deliberately a separate process from the engine, and the separation
// is the point. A matching engine has to be deterministic and auditable: the
// same input must produce the same output every time, and every fill must be
// explainable from the rules. A language model is none of those things, so it
// is not allowed anywhere near a matching decision, a price, or a fill.
//
// What it does here is the job a compliance analyst actually does: take
// structured detections the engine already computed and write up what they
// mean. Every figure in the narrative comes from the JSON below. The model is
// told it may not invent numbers, and the engine's own deterministic report
// remains the source of truth. With no API key configured the detections are
// still complete and still correct; only the prose is missing.

export const runtime = "edge";

interface Finding {
  readonly alert: string;
  readonly severity: number;
  readonly count: number;
  readonly detail: string;
  readonly basis: string;
}

interface Body {
  readonly symbol?: string;
  readonly stats?: Record<string, number>;
  readonly findings?: readonly Finding[];
  readonly book?: { readonly bid: number; readonly ask: number; readonly spread: number };
}

const SYSTEM = `You are a market surveillance analyst at a trading venue, writing the first-pass case note that goes to a compliance reviewer.

You will be given JSON produced by a deterministic C++ surveillance module running on a live order book. Write the case note.

Hard rules, in order of importance:

1. Every number you write must appear in the JSON. Do not compute new figures, do not estimate, do not round into a different number. If you want to say something you cannot source, say that it cannot be determined from this data.
2. Do not assert intent. Manipulation is defined by intent and intent is not observable in an order feed. Write what the pattern IS and what it is CONSISTENT WITH. "Consistent with layering" is correct; "the participant was spoofing" is not.
3. Respect the basis field. Where it says a signal is indicative only and not a violation on its own, say so plainly rather than burying it.
4. If the evidence is weak, say it is weak. A case note that overstates gets a reviewer in trouble.

Format, no headings, no bullets, no preamble:

Paragraph 1: what the detector saw, with the figures.
Paragraph 2: which recognised abuse pattern this resembles, and what would have to be true for it to be that pattern rather than ordinary market making.
Paragraph 3: the specific next check a reviewer should run to separate the two.

Under 200 words. Plain sentences. No em dashes.`;

export async function POST(req: Request): Promise<Response> {
  const key = process.env.ANTHROPIC_API_KEY;
  if (!key) {
    return new Response(
      "No ANTHROPIC_API_KEY is configured, so the narrative stage is off.\n\n" +
        "This is a supported state, not a failure. The detections beside this " +
        "panel are produced by the engine itself and are complete without it. " +
        "This stage only rewrites those same findings as prose for a reviewer.",
      { status: 200, headers: { "content-type": "text/plain; charset=utf-8" } },
    );
  }

  let body: Body;
  try {
    body = (await req.json()) as Body;
  } catch {
    return new Response("malformed request", { status: 400 });
  }
  if (!body.findings || body.findings.length === 0) {
    return new Response("Nothing has been detected yet. Let the book run.", {
      status: 200,
      headers: { "content-type": "text/plain; charset=utf-8" },
    });
  }

  const upstream = await fetch("https://api.anthropic.com/v1/messages", {
    method: "POST",
    headers: {
      "content-type": "application/json",
      "x-api-key": key,
      "anthropic-version": "2023-06-01",
    },
    body: JSON.stringify({
      model: "claude-sonnet-5",
      max_tokens: 700,
      system: SYSTEM,
      stream: true,
      messages: [
        {
          role: "user",
          content:
            "Surveillance output follows. Write the case note.\n\n" +
            JSON.stringify(
              { symbol: body.symbol, stats: body.stats, book: body.book, findings: body.findings },
              null,
              1,
            ),
        },
      ],
    }),
  });

  if (!upstream.ok || upstream.body === null) {
    const detail = await upstream.text().catch(() => "");
    return new Response(
      `The narrative stage could not reach the model (${upstream.status}). ` +
        `The detections beside this panel are unaffected.\n\n${detail.slice(0, 300)}`,
      { status: 200, headers: { "content-type": "text/plain; charset=utf-8" } },
    );
  }

  // Unwrap the SSE envelope so the client receives plain text it can append.
  const decoder = new TextDecoder();
  const encoder = new TextEncoder();
  const reader = upstream.body.getReader();
  let carry = "";

  const stream = new ReadableStream<Uint8Array>({
    async pull(controller) {
      const { done, value } = await reader.read();
      if (done) {
        controller.close();
        return;
      }
      carry += decoder.decode(value, { stream: true });
      const lines = carry.split("\n");
      carry = lines.pop() ?? "";
      for (const line of lines) {
        if (!line.startsWith("data:")) continue;
        const raw = line.slice(5).trim();
        if (raw === "" || raw === "[DONE]") continue;
        try {
          const ev = JSON.parse(raw) as {
            type?: string;
            delta?: { type?: string; text?: string };
          };
          if (ev.type === "content_block_delta" && ev.delta?.text)
            controller.enqueue(encoder.encode(ev.delta.text));
        } catch {
          // A partial frame; the carry buffer picks it up next round.
        }
      }
    },
    cancel() {
      void reader.cancel();
    },
  });

  return new Response(stream, {
    headers: {
      "content-type": "text/plain; charset=utf-8",
      "cache-control": "no-store",
    },
  });
}
