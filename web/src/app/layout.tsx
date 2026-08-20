import type { Metadata } from "next";
import { Fraunces, Archivo, Azeret_Mono } from "next/font/google";
import "./globals.css";

// Fraunces carries the page: a variable serif with enough character to sound
// like a document rather than a dashboard. Used at display sizes only.
const display = Fraunces({
  subsets: ["latin"],
  variable: "--font-display",
  axes: ["SOFT", "WONK", "opsz"],
  display: "swap",
});

const body = Archivo({
  subsets: ["latin"],
  variable: "--font-body",
  display: "swap",
});

// Every number on this page is data from a running engine, so the mono is a
// primary face here rather than a fallback for code blocks.
const mono = Azeret_Mono({
  subsets: ["latin"],
  variable: "--font-mono",
  display: "swap",
});

export const metadata: Metadata = {
  title: "pricetime, a matching engine, and its proof",
  description:
    "A limit order book and matching engine in C++20, compiled to WebAssembly. Two independent implementations, byte-identical output, running in your browser.",
};

export default function RootLayout({
  children,
}: Readonly<{ children: React.ReactNode }>) {
  return (
    <html lang="en" className={`${display.variable} ${body.variable} ${mono.variable}`}>
      <body className="antialiased">{children}</body>
    </html>
  );
}
