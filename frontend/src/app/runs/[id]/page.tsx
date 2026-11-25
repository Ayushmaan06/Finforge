import { Suspense } from "react";

import { Loading } from "@/components/ui";

import RunView from "./run-view";

// The client view reads the route params; Suspense lets the rest of the route prerender.
export default function Page({ params }: { params: Promise<{ id: string }> }) {
  return (
    <Suspense fallback={<Loading />}>
      <RunView params={params} />
    </Suspense>
  );
}
