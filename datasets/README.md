# datasets/

- `fixtures/`: small synthetic datasets for tests, in Binance file layouts, including deliberately
  malformed ones. Regenerate them with `python scripts/make_fixtures.py`. The findings each one must
  produce are listed in [docs/data.md](../docs/data.md) §5.
- `specs/`: dataset specs (registry, sources, validation policies) for the PLAN.md D5 data.
- Everything else (`raw/` downloads, `normalized/` datasets) is gitignored. Fetch raw data with
  `python scripts/fetch_binance.py`. Check Binance's terms before redistributing anything.
