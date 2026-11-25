import { Suspense } from "react";

import { Loading } from "@/components/ui";

import DatasetView from "./dataset-view";

// The client view reads the route params; Suspense lets the rest of the route prerender.
export default function Page({ params }: { params: Promise<{ id: string }> }) {
  return (
    <Suspense fallback={<Loading />}>
      <DatasetView params={params} />
    </Suspense>
  );
}
