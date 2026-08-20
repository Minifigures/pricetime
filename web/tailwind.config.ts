import type { Config } from "tailwindcss";

const config: Config = {
  content: ["./src/**/*.{ts,tsx}"],
  theme: {
    extend: {
      fontFamily: {
        display: ["var(--font-display)", "Georgia", "serif"],
        body: ["var(--font-body)", "system-ui", "sans-serif"],
        mono: ["var(--font-mono)", "ui-monospace", "monospace"],
      },
      colors: {
        paper: "var(--paper)",
        paper2: "var(--paper-2)",
        ink: "var(--ink)",
        ink2: "var(--ink-2)",
        ink3: "var(--ink-3)",
        rule: "var(--rule)",
        rule2: "var(--rule-2)",
        instrument: "var(--instrument)",
        instRule: "var(--inst-rule)",
        instInk: "var(--inst-ink)",
        bid: "var(--bid)",
        ask: "var(--ask)",
        bidLit: "var(--bid-lit)",
        askLit: "var(--ask-lit)",
        signal: "var(--signal)",
        signalLit: "var(--signal-lit)",
      },
    },
  },
  plugins: [],
};
export default config;
