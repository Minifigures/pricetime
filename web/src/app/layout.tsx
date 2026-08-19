import type { Metadata } from "next";
import "./globals.css";

export const metadata: Metadata = {
  title: "pricetime — a matching engine you can run in your browser",
  description:
    "A limit order book and matching engine in C++20, compiled to WebAssembly. Validated against 50 million real exchange orders.",
};

export default function RootLayout({
  children,
}: Readonly<{ children: React.ReactNode }>) {
  return (
    <html lang="en">
      <body className="antialiased">{children}</body>
    </html>
  );
}
