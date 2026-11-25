import { Suspense } from "react";

import { Loading } from "@/components/ui";

import NewBacktest from "./new-backtest";

export default function Page() {
  return (
    <>
      <h1>New backtest</h1>
      <Suspense fallback={<Loading />}>
        <NewBacktest />
      </Suspense>
    </>
  );
}
