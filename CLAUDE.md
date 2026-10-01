# AI Search PGVector — Dev Notes

Vector storage backend for the `ai_search` framework (see `../ai_search/CLAUDE.md` for the overall architecture). This module is its own git repo/project (`backdrop-contrib/ai_search_provider_pgvector`), a sibling of `ai_search`, not a submodule.

See `README.md` in this directory for requirements and server configuration (schema/table/metric/IVFFlat lists) — don't duplicate that here.

## Code Map

- `ai_search_pgvector.module` — registers the `pgvector` Search API service backend
- `includes/PgvectorService.inc` — `PgvectorService extends SearchApiAbstractService implements AiSearchBackendInterface`. Uses the site's existing PostgreSQL database connection directly (no separate vector client class) — talks to the `vector` extension via raw SQL (`<->`/`<=>`/`<#>` operators depending on metric, `IVFFLAT` index).

## Known Constraints

- Requires the `pgvector` PostgreSQL extension (`CREATE EXTENSION IF NOT EXISTS vector;`).
- Vector table + IVFFlat index auto-created on first index run; run `ANALYZE` after indexing for IVFFlat list count changes to take effect.
- `ai_search_provider_mariadb` is a direct port of this backend's approach — if fixing a bug here, check whether the same bug exists there.

**Last Updated:** 2026-07-07 by Claude
