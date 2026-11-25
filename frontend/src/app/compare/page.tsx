import { Suspense } from "react";

import { Loading } from "@/components/ui";

import CompareView from "./compare-view";

export default function Page() {
  return (
    <>
      <h1>Compare runs</h1>
      <Suspense fallback={<Loading />}>
        <CompareView />
      </Suspense>
    </>
  );
}
