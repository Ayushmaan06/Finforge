import type { NextConfig } from "next";

const nextConfig: NextConfig = {
  // The Docker image runs the standalone server (frontend/Dockerfile); `next start` needs the default.
  output: process.env.NEXT_OUTPUT === "standalone" ? "standalone" : undefined,
  cacheComponents: true,
  partialPrefetching: true,
};

export default nextConfig;
