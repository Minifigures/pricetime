import type { Config } from "tailwindcss";

const config: Config = {
  content: ["./src/**/*.{ts,tsx}"],
  theme: {
    extend: {
      fontFamily: { mono: ["ui-monospace", "SFMono-Regular", "Menlo", "monospace"] },
      colors: {
        ink: "#07090c",
        panel: "#0d1117",
        line: "#1c2430",
        bid: "#2ea043",
        ask: "#f85149",
      },
    },
  },
  plugins: [],
};
export default config;
