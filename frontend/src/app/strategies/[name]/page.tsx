import { Suspense } from "react";

import { Loading } from "@/components/ui";

import StrategyView from "./strategy-view";

// The client view reads the route params; Suspense lets the rest of the route prerender.
export default function Page({ params }: { params: Promise<{ name: string }> }) {
  return (
    <Suspense fallback={<Loading />}>
      <StrategyView params={params} />
    </Suspense>
  );
}
