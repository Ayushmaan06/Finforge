import type { Metadata } from "next";
import Link from "next/link";

import "./globals.css";

export const metadata: Metadata = {
  title: "FinForge",
  description: "Market simulation and backtesting research dashboard",
};

const NAV = [
  ["/", "Overview"],
  ["/datasets", "Datasets"],
  ["/strategies", "Strategies"],
  ["/runs/new", "New backtest"],
  ["/compare", "Compare"],
] as const;

export default function RootLayout({ children }: { children: React.ReactNode }) {
  return (
    <html lang="en">
      <body>
        <header className="topbar">
          <Link href="/" className="brand">
            FinForge
          </Link>
          <nav aria-label="Main">
            {NAV.map(([href, label]) => (
              <Link key={href} href={href}>
                {label}
              </Link>
            ))}
          </nav>
        </header>
        <main className="page">{children}</main>
      </body>
    </html>
  );
}
