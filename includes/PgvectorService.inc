<?php
/**
 * @file
 * PostgreSQL + pgvector Search API backend for Backdrop CMS.
 */

require_once backdrop_get_path('module', 'ai_search') . '/includes/AiSearchBackendPluginBase.inc';
require_once backdrop_get_path('module', 'ai_search') . '/includes/AiSearchBackendInterface.inc';
require_once backdrop_get_path('module', 'ai_search') . '/includes/AiSearchBackend.inc';

class PgvectorService extends SearchApiAbstractService implements AiSearchBackendInterface {

  const TABLE_PREFIX = 'search_vector_';

  /**
   * Minimum row count before an IVFFLAT approximate index is worth creating.
   * Below this, exact sequential scan is fast enough and always correct.
   */
  const IVFFLAT_MIN_ROWS = 10000;

  protected $configuration;

  /**
   * Lazily-opened PDO connection to the pgvector PostgreSQL server.
   *
   * This backend connects to its own PostgreSQL server (like the Milvus
   * backend connects to its own Milvus server). It does NOT use the site's
   * primary database, which Backdrop does not support running on PostgreSQL.
   *
   * @var PDO|null
   */
  protected $pdo;

  /**
   * Qualified table names already verified/created in this request, so
   * ensureTable() doesn't re-run CREATE SCHEMA/EXTENSION and index-existence
   * checks on every indexItems() batch within the same request.
   *
   * @var array
   */
  protected $tablesEnsured = [];

  public function __construct($configuration = []) {
    parent::__construct($configuration);
    if (is_object($configuration) && property_exists($configuration, 'options')) {
      $this->configuration = (array) $configuration->options;
    }
    elseif (is_array($configuration)) {
      $this->configuration = $configuration;
    }
    else {
      $this->configuration = [];
    }
  }

  public function defaultConfiguration() {
    return parent::defaultConfiguration() + [
      // Connection to the dedicated pgvector PostgreSQL server.
      'host'          => '',
      'port'          => '5432',
      'database'      => '',
      'username'      => '',
      'password'      => '',
      'connect_timeout' => 5,
      'sslmode'       => '',
      'schema'        => 'public',
      'table'         => '', // default to computed per-index table name
      'metric_type'   => 'COSINE', // COSINE | L2 | IP
      'ivfflat_lists' => 100,
    ];
  }

  /**
   * Returns the PDO connection to the pgvector server, opening it if needed.
   */
  protected function connection() {
    if ($this->pdo instanceof PDO) {
      return $this->pdo;
    }
    $host = !empty($this->options['host']) ? $this->options['host'] : '';
    $port = !empty($this->options['port']) ? $this->options['port'] : '5432';
    $database = !empty($this->options['database']) ? $this->options['database'] : '';
    $username = !empty($this->options['username']) ? $this->options['username'] : '';
    $password = isset($this->options['password']) ? $this->options['password'] : '';
    $connect_timeout = !empty($this->options['connect_timeout']) ? (int) $this->options['connect_timeout'] : 5;
    $sslmode = !empty($this->options['sslmode']) ? $this->options['sslmode'] : '';

    if ($host === '' || $database === '') {
      throw new Exception('pgvector backend is not configured: host and database are required.');
    }

    $dsn = sprintf('pgsql:host=%s;port=%s;dbname=%s;connect_timeout=%d', $host, $port, $database, $connect_timeout);
    if ($sslmode !== '') {
      $dsn .= ';sslmode=' . $sslmode;
    }
    $this->pdo = new PDO($dsn, $username, $password, [
      PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
      PDO::ATTR_DEFAULT_FETCH_MODE => PDO::FETCH_OBJ,
    ]);
    return $this->pdo;
  }

  /**
   * Runs a parameterized statement against the pgvector server.
   *
   * Drop-in replacement for db_query() for this backend: accepts the same
   * :named placeholder + args array style already used throughout this class.
   *
   * @return PDOStatement
   */
  protected function execSql($sql, array $args = []) {
    $stmt = $this->connection()->prepare($sql);
    $stmt->execute($args);
    return $stmt;
  }

  public function configurationForm($form, &$form_state) {
    $embedding_configuration = ai_search_default_engine_configuration();
    if (!empty($this->options['embeddings_engine'])) {
      $embedding_configuration['embeddings_engine'] = $this->options['embeddings_engine'];
    }
    if (!empty($this->options['embeddings_engine_configuration'])) {
      $embedding_configuration['embeddings_engine_configuration'] = $this->options['embeddings_engine_configuration'];
    }
    $form = ai_search_engine_configuration_form($form, $form_state, $embedding_configuration);

    $form['host'] = [
      '#type' => 'textfield',
      '#title' => t('Host'),
      '#required' => TRUE,
      '#description' => t('The PostgreSQL server to connect to (e.g. the pgvector service name in DDEV, or a remote host).'),
      '#default_value' => !empty($this->options['host']) ? $this->options['host'] : '',
    ];
    $form['port'] = [
      '#type' => 'textfield',
      '#title' => t('Port'),
      '#required' => TRUE,
      '#default_value' => !empty($this->options['port']) ? $this->options['port'] : '5432',
    ];
    $form['database'] = [
      '#type' => 'textfield',
      '#title' => t('Database'),
      '#required' => TRUE,
      '#description' => t('The PostgreSQL database name.'),
      '#default_value' => !empty($this->options['database']) ? $this->options['database'] : '',
    ];
    $form['username'] = [
      '#type' => 'textfield',
      '#title' => t('Username'),
      '#required' => TRUE,
      '#default_value' => !empty($this->options['username']) ? $this->options['username'] : '',
    ];
    $form['password'] = [
      '#type' => 'password',
      '#title' => t('Password'),
      '#description' => t('Leave blank to keep the currently saved password.'),
    ];
    $form['connect_timeout'] = [
      '#type' => 'number',
      '#title' => t('Connection timeout (seconds)'),
      '#description' => t('How long to wait when connecting to a remote server before giving up.'),
      '#default_value' => !empty($this->options['connect_timeout']) ? (int) $this->options['connect_timeout'] : 5,
      '#min' => 1,
    ];
    $form['sslmode'] = [
      '#type' => 'select',
      '#title' => t('SSL mode'),
      '#description' => t('PostgreSQL sslmode connection parameter. Leave as default for local/trusted connections.'),
      '#default_value' => !empty($this->options['sslmode']) ? $this->options['sslmode'] : '',
      '#options' => [
        '' => t('- Default -'),
        'disable' => 'disable',
        'allow' => 'allow',
        'prefer' => 'prefer',
        'require' => 'require',
        'verify-ca' => 'verify-ca',
        'verify-full' => 'verify-full',
      ],
    ];

    $form['schema'] = [
      '#type' => 'textfield',
      '#title' => t('Schema'),
      '#description' => t('PostgreSQL schema to use.'),
      '#default_value' => !empty($this->options['schema']) ? $this->options['schema'] : 'public',
    ];
    $form['table'] = [
      '#type' => 'textfield',
      '#title' => t('Table name (optional)'),
      '#description' => t('Leave empty to auto-generate per index, prefixed with @p.', ['@p' => self::TABLE_PREFIX]),
      '#default_value' => !empty($this->options['table']) ? $this->options['table'] : '',
      '#pattern' => '[a-zA-Z0-9_]*',
    ];
    $form['metric_type'] = [
      '#type' => 'select',
      '#title' => t('Metric Type'),
      '#description' => t('Distance metric for nearest-neighbor search.'),
      '#default_value' => !empty($this->options['metric_type']) ? $this->options['metric_type'] : 'COSINE',
      '#required' => TRUE,
      '#options' => [
        'L2' => 'L2',
        'IP' => 'IP',
        'COSINE' => 'COSINE',
      ],
    ];
    $form['ivfflat_lists'] = [
      '#type' => 'number',
      '#title' => t('IVFFLAT lists'),
      '#description' => t('Number of inverted lists for the index (requires ANALYZE).'),
      '#default_value' => !empty($this->options['ivfflat_lists']) ? (int) $this->options['ivfflat_lists'] : 100,
      '#min' => 1,
    ];

    $form['help'] = [
      '#markup' => '<p>' . t('This backend connects to a dedicated PostgreSQL server with the pgvector extension (it does not use the site database). The extension is created automatically on first index if the configured user has sufficient privileges (CREATE EXTENSION IF NOT EXISTS vector;).') . '</p>',
    ];

    $form = ai_search_reranking_configuration_form($form, $form_state, $this->options);
    return $form;
  }

  public function configurationFormSubmit(array $form, array &$values, array &$form_state) {
    // Preserve the existing password when the field is left blank.
    if (empty($values['password']) && !empty($this->options['password'])) {
      unset($values['password']);
    }
    parent::configurationFormSubmit($form, $values, $form_state);
  }

  /**
   * Index items into pgvector table.
   */
  public function indexItems(SearchApiIndex $index, array $items) {
    global $ai_search_is_indexing;
    $ai_search_is_indexing = TRUE;

    if (empty($items)) {
      return [];
    }

    $dimension = isset($this->options['embeddings_engine_configuration']['dimension'])
      ? (int) $this->options['embeddings_engine_configuration']['dimension']
      : ai_search_get_embedding_dimension();

    $table = $this->getTableName($index);
    $schema = $this->getSchema();
    $this->ensureTable($schema, $table, $dimension);

    $expected_dimension = $dimension;

    $successful_ids = [];

    // Build a map of entity_type => [ids] from $items.
    //
    // Search API keys $items by the item ID. For an entity index this is the
    // raw entity ID (e.g. "192"), and the entity type comes from the index
    // itself. Some configurations instead use a canonical/compound key like
    // "entity:node/192:en" or "node/192"; handle those too. The item VALUE is
    // a field array and does not carry ->nid/->tid/->item_id, so those object
    // properties are only a last-resort fallback.
    $index_entity_type = $this->getIndexEntityType($index);
    $ids_by_type = [];
    foreach ($items as $id => $item) {
      $entity_type = NULL;
      $entity_id = NULL;

      if (is_string($id) && strpos($id, '/') !== FALSE) {
        // Compound key: "entity:node/192:en" or "node/192".
        $raw = (strncmp($id, 'entity:', 7) === 0) ? substr($id, 7) : $id;
        list($entity_type, $id_part) = explode('/', $raw, 2);
        // Strip any language/revision suffix (e.g. "192:en" => "192").
        $entity_id = (int) (strpos($id_part, ':') !== FALSE ? strstr($id_part, ':', TRUE) : $id_part);
      }
      elseif (is_numeric($id) && $index_entity_type) {
        // Plain entity-ID key; entity type is the index's item type.
        $entity_type = $index_entity_type;
        $entity_id = (int) $id;
      }
      else {
        if (is_array($item)) $item = (object) $item;
        if (isset($item->nid)) {
          $entity_type = 'node';
          $entity_id = is_array($item->nid) && isset($item->nid['value']) ? (int) $item->nid['value'] : (int) $item->nid;
        }
        elseif (isset($item->tid)) {
          $entity_type = 'taxonomy_term';
          $entity_id = is_array($item->tid) && isset($item->tid['value']) ? (int) $item->tid['value'] : (int) $item->tid;
        }
        elseif (isset($item->item_id)) {
          $entity_type = 'paragraphs_item';
          $entity_id = is_array($item->item_id) && isset($item->item_id['value']) ? (int) $item->item_id['value'] : (int) $item->item_id;
        }
      }

      if ($entity_type && $entity_id) {
        $ids_by_type[$entity_type][] = $entity_id;
      }
    }

    foreach ($ids_by_type as $entity_type => $ids) {
      foreach (array_unique($ids) as $entity_id) {
        $rows = ai_search_normalize_vectors($entity_type, $entity_id, $expected_dimension, $index->machine_name);
        if (empty($rows)) {
          continue;
        }
        foreach ($rows as $r) {
          // Compose stable long id.
          $long_id = "entity:{$entity_type}/{$entity_id}:en:{$r['field_name']}:{$r['chunk_index']}";
          $backdrop_entity_id = "entity:{$entity_type}/{$entity_id}:en";
          $nid = ($entity_type === 'node') ? (int) $entity_id : NULL;

          $values = array_map('floatval', $r['values']);
          $vector_literal = '[' . implode(',', $values) . ']';

          // Mirror AIMilvusService::indexItems(): keep all contextual fields
          // from $r['metadata'] (title, url, taxonomy, etc.), not just content.
          $metadata = array_merge(
            $r['metadata'] ?? [],
            [
              'content'         => $r['metadata']['content'] ?? '',
              'title'           => $r['metadata']['title'] ?? NULL,
              'search_api_url'  => $r['metadata']['search_api_url'] ?? NULL,
              'url'             => $r['metadata']['url'] ?? NULL,
            ]
          );
          $metadata_json = json_encode(array_filter($metadata, function ($v) { return $v !== NULL; }));

          // Use plain SQL for upsert.
          $qualified = $this->qualify($schema, $table);
          $sql = "INSERT INTO {$qualified} (id, backdrop_entity_id, nid, content, metadata, vector) VALUES (:id, :beid, :nid, :content, :metadata, CAST(:vector AS vector)) "
               . "ON CONFLICT (id) DO UPDATE SET backdrop_entity_id = EXCLUDED.backdrop_entity_id, nid = EXCLUDED.nid, content = EXCLUDED.content, metadata = EXCLUDED.metadata, vector = EXCLUDED.vector";

          $this->execSql($sql, [
            ':id' => $long_id,
            ':beid' => $backdrop_entity_id,
            ':nid' => $nid,
            ':content' => isset($r['metadata']['content']) ? (string) $r['metadata']['content'] : '',
            ':metadata' => $metadata_json,
            ':vector' => $vector_literal,
          ]);
          $successful_ids[] = $entity_id;
        }
      }
    }

    return array_unique($successful_ids);
  }

  /**
   * Delete items from pgvector table.
   */
  public function deleteItems($ids = 'all', ?SearchApiIndex $index = NULL) {
    $table = $this->getTableName($index);
    $schema = $this->getSchema();
    $qualified = $this->qualify($schema, $table);

    if ($ids === 'all') {
      $this->execSql("TRUNCATE TABLE {$qualified}");
      return;
    }

    $ids = is_array($ids) ? array_filter($ids) : [];
    if (empty($ids)) {
      return;
    }

    // Build list of backdrop_entity_id values to delete, using the same
    // entity type resolution as indexItems() rather than assuming node.
    $entity_type = $this->getIndexEntityType($index) ?: 'node';
    $beids = [];
    foreach ($ids as $id) {
      $id = (int) $id;
      $beids[] = "entity:{$entity_type}/{$id}:en";
    }
    $placeholders = [];
    $args = [];
    foreach ($beids as $i => $beid) {
      $ph = ":b{$i}";
      $placeholders[] = $ph;
      $args[$ph] = $beid;
    }
    $sql = "DELETE FROM {$qualified} WHERE backdrop_entity_id IN (" . implode(',', $placeholders) . ")";
    $this->execSql($sql, $args);
  }

  /**
   * Perform a nearest-neighbor search using pgvector.
   */
  public function search(SearchApiQueryInterface $query) {
    $index = $query->getIndex();
    $index_options = isset($index->options) ? $index->options : [];

    // Model selection: server options (canonical) → service embedding_model fallback.
    $model = $this->options['embeddings_engine'] ?? $this->options['embedding_model'] ?? NULL;

    if (empty($model)) {
      watchdog('ai_search_pgvector', 'No model configured!', [], WATCHDOG_ERROR);
      return ['result count' => 0, 'results' => []];
    }

    // Extract search text from keys.
    $keys = $query->getKeys();
    $text = is_array($keys)
      ? implode(' ', array_filter($keys, function ($key) { return is_string($key) && $key !== 'AND' && $key !== '#conjunction'; }))
      : (string) $keys;
    $text = trim($text);
    if ($text === '') {
      return ['result count' => 0, 'results' => []];
    }

    // Resolve client and request embedding using compatibility helpers.
    $api = NULL; $stripped_model = NULL; $model_to_use = NULL;
    $api = ai_get_api_for_model($model, $stripped_model);
    $model_to_use = !empty($stripped_model) ? $stripped_model : $model;

    if ($api && method_exists($api, 'recordLog')) {
      $api->recordLog('search_embedding', $model, ['text_len' => strlen($text)], NULL, TRUE, 0);
    }

    if (!$api) {
      watchdog('ai_search_pgvector', 'Failed to resolve API client for model: @model', ['@model' => $model], WATCHDOG_ERROR);
      throw new Exception('Failed to get API client for model: ' . $model);
    }

    $query_embedding = $api->embedding(ai_search_apply_embedding_task_prefix($text, $model_to_use, 'query'), $model_to_use);
    if (empty($query_embedding)) {
      throw new Exception('Failed to retrieve embedding.');
    }

    $top_k = $query->getOption('top_k') ?? 8;
    return $this->queryVector($index, $query_embedding, $top_k, $text, $this->getIndexEntityType($index));
  }

  /**
   * Run a similarity search against an already-computed embedding vector.
   *
   * Factored out of search() so RAG callers (e.g. ai_search's generic
   * backend dispatch used by ai_agents/ai_assistants tools) can query with a
   * vector they already generated, without needing a SearchApiQueryInterface.
   *
   * @param SearchApiIndex $index
   *   The index to query.
   * @param array $vector
   *   The precomputed query embedding.
   * @param int $top_k
   *   Number of results to return.
   * @param string $rerank_text
   *   Original query text, used for metadata reranking (optional).
   * @param string|null $entity_type_for_ids
   *   Entity type to compare against for item-id shortening; defaults to the
   *   index's own entity type.
   *
   * @return array
   *   ['result count' => int, 'results' => array].
   */
  public function queryVector(SearchApiIndex $index, array $vector, $top_k = 8, $rerank_text = '', $entity_type_for_ids = NULL) {
    $top_k = (int) $top_k;
    $metric = !empty($this->options['metric_type']) ? $this->options['metric_type'] : 'COSINE';
    $operator = $this->metricOperator($metric);

    $table = $this->getTableName($index);
    $schema = $this->getSchema();
    $qualified = $this->qualify($schema, $table);

    $vector_literal = '[' . implode(',', array_map('floatval', $vector)) . ']';

    $sql = "SELECT *, (vector {$operator} CAST(:q AS vector)) AS distance FROM {$qualified} ORDER BY vector {$operator} CAST(:q AS vector) LIMIT " . $top_k;
    $result = $this->execSql($sql, [
      ':q' => $vector_literal,
    ]);

    // Collect DB rows and normalize for reranking.
    $normalized_matches = [];
    foreach ($result as $row) {
      $row_array = (array) $row;
      $metadata = array_diff_key($row_array, array_flip(['vector', 'distance', 'metadata']));
      // Stored contextual fields (title, url, taxonomy, etc.) take precedence
      // over the raw table columns they were derived from.
      if (!empty($row->metadata)) {
        $decoded = json_decode($row->metadata, TRUE);
        if (is_array($decoded)) {
          $metadata = array_merge($metadata, $decoded);
        }
      }
      $normalized_matches[] = [
        'score'    => $this->distanceToSimilarity((float) $row->distance, $metric),
        'metadata' => $metadata,
        '_raw'     => $row,
      ];
    }

    $rerank_settings = !empty($this->options['metadata_reranking']) ? $this->options['metadata_reranking'] : [];
    ai_search_apply_metadata_reranking($normalized_matches, $rerank_text, $rerank_settings);

    $entity_type_for_ids = $entity_type_for_ids ?: $this->getIndexEntityType($index);
    $results = [];
    foreach ($normalized_matches as $nm) {
      $row = $nm['_raw'];
      $entity_type = NULL; $entity_id = NULL;
      if (!empty($row->backdrop_entity_id) && preg_match('/^entity:([^\/]+)\/(\d+)/', $row->backdrop_entity_id, $m)) {
        $entity_type = $m[1];
        $entity_id = $m[2];
      }
      elseif (!empty($row->nid)) {
        $entity_type = 'node';
        $entity_id = $row->nid;
      }
      if ($entity_type && $entity_id) {
        $item_id = $entity_type == $entity_type_for_ids ? $entity_id : "{$entity_type}/{$entity_id}";
        $results[$item_id] = [
          'id'      => $item_id,
          'score'   => $nm['score'],
          'fields'  => [],
          'entity'  => entity_load($entity_type, $entity_id),
          'excerpt' => isset($row->content) ? AIContentExtractor::htmlToMarkdown($row->content) : '',
          'metadata' => $nm['metadata'],
        ];
      }
    }

    return [
      'result count' => count($results),
      'results' => $results,
    ];
  }

  public function supportsDataType($type) {
    return in_array($type, ['embeddings']);
  }

  // Embedding engine helpers required by interface.
  public function loadEmbeddingsEngine() {
    return ai_search_load_embeddings_engine($this->configuration);
  }
  public function getEmbeddingsEngine() {
    return isset($this->configuration['embeddings_engine']) ? $this->configuration['embeddings_engine'] : '';
  }
  public function getEmbeddingEnginesOptions() {
    return ai_search_get_embedding_engines_options();
  }
  public function updateEmbeddingConfigurationForm($form, &$form_state) {
    return ai_search_update_embedding_configuration_form($form, $form_state);
  }

  // Internal helpers.
  protected function getSchema() {
    return !empty($this->options['schema']) ? $this->options['schema'] : 'public';
  }
  protected function getTableName($index) {
    if (!empty($this->options['table'])) {
      return preg_replace('/[^a-zA-Z0-9_]/', '_', $this->options['table']);
    }
    if (!$index || empty($index->machine_name)) {
      throw new Exception('pgvector backend: cannot determine table name without an index or a configured table name.');
    }
    return preg_replace('/[^a-zA-Z0-9_]/', '_', self::TABLE_PREFIX . $index->machine_name);
  }
  protected function qualify($schema, $table) {
    $schema = preg_replace('/[^a-zA-Z0-9_]/', '', $schema);
    $table  = preg_replace('/[^a-zA-Z0-9_]/', '', $table);
    return $schema . '.' . $table;
  }

  /**
   * Resolve an index's entity type, tolerating index objects/stubs that
   * don't implement getEntityType() (e.g. bare stdClass used by some
   * callers).
   */
  protected function getIndexEntityType($index) {
    if ($index && method_exists($index, 'getEntityType')) {
      return $index->getEntityType();
    }
    return (!empty($index->item_type)) ? $index->item_type : NULL;
  }

  /**
   * Ensure pgvector extension, table and index exist with correct dimension.
   */
  protected function ensureTable($schema, $table, $dimension) {
    // Sanitize schema identifier to prevent SQL injection.
    $schema = preg_replace('/[^a-zA-Z0-9_]/', '', $schema);
    if ($schema === '') {
      $schema = 'public';
    }

    $qualified_cache_key = $this->qualify($schema, $table);
    if (!empty($this->tablesEnsured[$qualified_cache_key])) {
      // Already verified/created earlier in this request; skip the
      // repeated CREATE SCHEMA/EXTENSION and index checks below.
      return;
    }

    // Create schema if not exists. $schema is already sanitized to
    // [a-zA-Z0-9_] above, so it is safe to interpolate directly. (We must not
    // use db_escape_table(), which quotes with MySQL backticks.)
    $this->execSql('CREATE SCHEMA IF NOT EXISTS ' . $schema);
    // Enable extension.
    $this->execSql('CREATE EXTENSION IF NOT EXISTS vector');

    $qualified = $this->qualify($schema, $table);

    // Does the table exist?
    $exists = FALSE;
    try {
      $this->execSql('SELECT 1 FROM ' . $qualified . ' LIMIT 1');
      $exists = TRUE;
    }
    catch (\Exception $e) {
      $exists = FALSE;
    }

    if (!$exists) {
      $sql = 'CREATE TABLE ' . $qualified . ' (
        id VARCHAR(200) PRIMARY KEY,
        backdrop_entity_id VARCHAR(200),
        nid INTEGER NULL,
        content TEXT,
        metadata TEXT,
        vector VECTOR(' . (int) $dimension . ')
      )';
      $this->execSql($sql);
    }
    else {
      // Validate dimension; if wrong, fail loudly (dropping data would be risky).
      try {
        // For the pgvector VECTOR type, the dimension is stored directly in
        // atttypmod (unlike varchar, which uses atttypmod - 4).
        $dim = $this->execSql("SELECT atttypmod FROM pg_attribute WHERE attrelid = to_regclass(:rel) AND attname = 'vector'", [':rel' => $qualified])->fetchColumn();
      }
      catch (\Exception $e) {
        $dim = NULL;
      }
      // If dimension is different, fail fast — proceeding would corrupt stored vectors.
      if ($dim && (int) $dim !== (int) $dimension) {
        throw new \Exception(sprintf('Existing table %s vector dimension (%d) differs from configured (%d).', $qualified, (int) $dim, (int) $dimension));
      }

      // Backfill the metadata column on tables created before it existed.
      $has_metadata_col = $this->execSql("SELECT 1 FROM information_schema.columns WHERE table_schema = :s AND table_name = :t AND column_name = 'metadata'", [':s' => $schema, ':t' => $table])->fetchColumn();
      if (!$has_metadata_col) {
        $this->execSql('ALTER TABLE ' . $qualified . ' ADD COLUMN metadata TEXT');
      }
    }

    // IVFFLAT is an approximate index: recall degrades badly when the row
    // count doesn't justify the number of lists (pgvector's own guidance is
    // lists ~= rows/1000 for tables under 1M rows). Below this threshold, an
    // exact sequential scan is both fast enough and always correct -- there's
    // no reason to trade accuracy for a speed-up the table is too small to
    // need. This was previously creating a lists=100 index on ~1500-row
    // tables (75 rows/list), which meant default probes=1 searched roughly
    // 1% of the table and silently missed genuinely relevant matches.
    //
    // Use pg_class.reltuples (a planner estimate, refreshed by ANALYZE/
    // autovacuum) rather than an exact COUNT(*), which requires a full table
    // scan and gets run on every indexItems() batch.
    $row_count = (int) $this->execSql('SELECT reltuples::bigint FROM pg_class WHERE oid = to_regclass(:rel)', [':rel' => $qualified])->fetchColumn();
    if ($row_count < self::IVFFLAT_MIN_ROWS) {
      $this->tablesEnsured[$qualified_cache_key] = TRUE;
      return;
    }

    // Create IVFFLAT index if missing.
    $metric = !empty($this->options['metric_type']) ? $this->options['metric_type'] : 'COSINE';
    $opclass = $this->metricOpclass($metric);
    $lists = !empty($this->options['ivfflat_lists']) ? (int) $this->options['ivfflat_lists'] : max(1, (int) round($row_count / 1000));
    $index_name = $table . '_ivfflat_idx';
    $exists_idx = $this->execSql("SELECT 1 FROM pg_indexes WHERE schemaname = :s AND tablename = :t AND indexname = :i", [':s' => $schema, ':t' => $table, ':i' => $index_name])->fetchColumn();
    if (!$exists_idx) {
      $this->execSql('CREATE INDEX ' . $index_name . ' ON ' . $qualified . ' USING ivfflat (vector ' . $opclass . ') WITH (lists = ' . (int) $lists . ')');
      // ANALYZE recommended for IVFFLAT.
      $this->execSql('ANALYZE ' . $qualified);
    }

    $this->tablesEnsured[$qualified_cache_key] = TRUE;
  }

  protected function metricOpclass($metric) {
    switch (strtoupper($metric)) {
      case 'L2': return 'vector_l2_ops';
      case 'IP': return 'vector_ip_ops';
      case 'COSINE':
      default: return 'vector_cosine_ops';
    }
  }
  protected function metricOperator($metric) {
    switch (strtoupper($metric)) {
      case 'IP': return '<#>';
      case 'COSINE': return '<=>';
      case 'L2':
      default: return '<->';
    }
  }

  /**
   * Convert a raw distance value into a "higher is better" similarity score.
   *
   * pgvector's distance operators return distance (lower = more similar),
   * but every caller of queryVector()/search() -- including
   * ai_search_search_block's score_threshold filter -- assumes score means
   * similarity (higher is a better match), matching how other backends like
   * milvus already report it. Returning raw distance silently inverts every
   * threshold/ranking decision downstream: the best matches get rejected as
   * "too low" and weak matches pass as "good enough".
   */
  protected function distanceToSimilarity($distance, $metric) {
    switch (strtoupper($metric)) {
      case 'IP':
        // <#> returns the negative inner product; negate back to get a
        // "higher is better" value.
        return -$distance;
      case 'L2':
        // Euclidean distance is unbounded; map it monotonically into (0, 1].
        return 1 / (1 + $distance);
      case 'COSINE':
      default:
        // <=> returns cosine distance (1 - cosine_similarity).
        return 1 - $distance;
    }
  }
}
