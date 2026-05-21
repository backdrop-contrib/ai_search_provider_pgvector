# AI Search PGVector

PostgreSQL pgvector provider for the Backdrop CMS AI Search module. Stores and queries embeddings directly in your existing PostgreSQL database using the pgvector extension, eliminating the need for a separate vector database service.

## Requirements

- Backdrop CMS 1.x
- `ai_search` module
- PostgreSQL with the [pgvector extension](https://github.com/pgvector/pgvector) installed
- The pgvector extension must be enabled in your database: `CREATE EXTENSION IF NOT EXISTS vector;`

## Installation

- Install this module using the official [Backdrop CMS instructions](https://backdropcms.org/user-guide/modules).

## Configuration

1. Enable this module.
2. Go to Admin -> Configuration -> Search and Metadata -> Search API and create or edit a server.
3. Select **PGVector** as the service backend.
4. Configure the server settings:
   - **Schema** — PostgreSQL schema to store vector tables in. Defaults to `public`.
   - **Table name** — Optional. Leave empty to auto-generate a per-index table name prefixed with `search_vector_`.
   - **Metric Type** — Distance metric for nearest-neighbor search: `COSINE` (default), `L2`, or `IP`.
   - **IVFFLAT lists** — Number of inverted lists for the IVFFlat index. Defaults to `100`. Run `ANALYZE` on the table after indexing for this setting to take effect.
   - **Embeddings engine** — Select the embedding model to use for generating vectors.
5. Create a Search API index on that server.
6. Index your content to populate the vector table.

## Notes

- This backend uses your site's existing database connection — no additional database credentials are needed.
- The vector table and IVFFlat index are created automatically on first index run.
- The embedding dimension is derived from the selected embeddings engine configuration.

## Issues

Bugs and feature requests should be reported in the [Issue Queue](https://github.com/backdrop-contrib/ai_search_provider_pgvector/issues).

## Current Maintainer

[Justin Keiser](https://github.com/keiserjb)

## Credits

- Created for Backdrop CMS by [Justin Keiser](https://github.com/keiserjb).
- Developed with AI assistance.

## License

This project is GPL v2 software. See the LICENSE.txt file in this directory for complete text.
