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

  protected $configuration;

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
      // Uses the site's default database connection.
      'schema'        => 'public',
      'table'         => '', // default to computed per-index table name
      'metric_type'   => 'COSINE', // COSINE | L2 | IP
      'ivfflat_lists' => 100,
    ];
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
      '#markup' => '<p>' . t('This backend uses the site database. You must enable the pgvector extension (CREATE EXTENSION IF NOT EXISTS vector;) and ensure sufficient privileges.') . '</p>',
    ];

    $form = ai_search_reranking_configuration_form($form, $form_state, $this->options);
    return $form;
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

    // Build entities per type like Milvus implementation.
    $ids_by_type = [];
    foreach ($items as $item) {
      if (is_array($item)) $item = (object) $item;
      if (isset($item->nid)) {
        $ids_by_type['node'][] = is_array($item->nid) && isset($item->nid['value']) ? (int) $item->nid['value'] : (int) $item->nid;
      }
      elseif (isset($item->tid)) {
        $ids_by_type['taxonomy_term'][] = is_array($item->tid) && isset($item->tid['value']) ? (int) $item->tid['value'] : (int) $item->tid;
      }
      elseif (isset($item->item_id)) {
        $ids_by_type['paragraphs_item'][] = is_array($item->item_id) && isset($item->item_id['value']) ? (int) $item->item_id['value'] : (int) $item->item_id;
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

          // Use plain SQL for upsert.
          $qualified = $this->qualify($schema, $table);
          $sql = "INSERT INTO {$qualified} (id, backdrop_entity_id, nid, content, vector) VALUES (:id, :beid, :nid, :content, :vector::vector) "
               . "ON CONFLICT (id) DO UPDATE SET backdrop_entity_id = EXCLUDED.backdrop_entity_id, nid = EXCLUDED.nid, content = EXCLUDED.content, vector = EXCLUDED.vector";

          db_query($sql, [
            ':id' => $long_id,
            ':beid' => $backdrop_entity_id,
            ':nid' => $nid,
            ':content' => isset($r['metadata']['content']) ? (string) $r['metadata']['content'] : '',
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
  public function deleteItems($ids = 'all', SearchApiIndex $index = NULL) {
    $table = $this->getTableName($index);
    $schema = $this->getSchema();
    $qualified = $this->qualify($schema, $table);

    if ($ids === 'all') {
      db_query("TRUNCATE TABLE {$qualified}");
      return;
    }

    $ids = is_array($ids) ? array_filter($ids) : [];
    if (empty($ids)) {
      return;
    }

    // Build list of backdrop_entity_id values to delete.
    $beids = [];
    foreach ($ids as $id) {
      $id = (int) $id;
      $beids[] = "entity:node/{$id}:en"; // current indices generally use nodes; extend as needed.
    }
    $placeholders = [];
    $args = [];
    foreach ($beids as $i => $beid) {
      $ph = ":b{$i}";
      $placeholders[] = $ph;
      $args[$ph] = $beid;
    }
    $sql = "DELETE FROM {$qualified} WHERE backdrop_entity_id IN (" . implode(',', $placeholders) . ")";
    db_query($sql, $args);
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

    $query_embedding = $api->embedding($text, $model_to_use);
    if (empty($query_embedding)) {
      throw new Exception('Failed to retrieve embedding.');
    }

    $top_k = $query->getOption('top_k') ?? 8;
    $metric = !empty($this->options['metric_type']) ? $this->options['metric_type'] : 'COSINE';
    $operator = $this->metricOperator($metric);

    $table = $this->getTableName($index);
    $schema = $this->getSchema();
    $qualified = $this->qualify($schema, $table);

    $vector_literal = '[' . implode(',', array_map('floatval', $query_embedding)) . ']';

    $sql = "SELECT *, (vector {$operator} :q::vector) AS distance FROM {$qualified} ORDER BY vector {$operator} :q::vector LIMIT :limit";
    $result = db_query($sql, [
      ':q' => $vector_literal,
      ':limit' => (int) $top_k,
    ]);

    // Collect DB rows and normalize for reranking.
    $normalized_matches = [];
    foreach ($result as $row) {
      $row_array = (array) $row;
      $metadata = array_diff_key($row_array, array_flip(['vector', 'distance']));
      $normalized_matches[] = [
        'score'    => (float) $row->distance,
        'metadata' => $metadata,
        '_raw'     => $row,
      ];
    }

    $rerank_settings = !empty($this->options['metadata_reranking']) ? $this->options['metadata_reranking'] : [];
    ai_search_apply_metadata_reranking($normalized_matches, $text, $rerank_settings);

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
        $item_id = $entity_type == $query->getIndex()->getEntityType() ? $entity_id : "{$entity_type}/{$entity_id}";
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
    $base = !empty($this->options['table']) ? $this->options['table'] : self::TABLE_PREFIX . $index->machine_name;
    return preg_replace('/[^a-zA-Z0-9_]/', '_', $base);
  }
  protected function qualify($schema, $table) {
    $schema = preg_replace('/[^a-zA-Z0-9_]/', '', $schema);
    $table  = preg_replace('/[^a-zA-Z0-9_]/', '', $table);
    return $schema . '.' . $table;
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
    // Create schema if not exists.
    db_query('CREATE SCHEMA IF NOT EXISTS ' . db_escape_table($schema));
    // Enable extension.
    db_query('CREATE EXTENSION IF NOT EXISTS vector');

    $qualified = $this->qualify($schema, $table);

    // Does the table exist?
    $exists = FALSE;
    try {
      db_query('SELECT 1 FROM ' . $qualified . ' LIMIT 1');
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
        vector VECTOR(' . (int) $dimension . ')
      )';
      db_query($sql);
    }
    else {
      // Validate dimension; if wrong, fail loudly (dropping data would be risky).
      try {
        // For pgvector, dimension is stored in atttypmod; see pgvector docs: dim = atttypmod - 4.
        $dim = db_query("SELECT atttypmod - 4 FROM pg_attribute WHERE attrelid = :rel::regclass AND attname = 'vector'", [':rel' => $qualified])->fetchField();
      }
      catch (\Exception $e) {
        $dim = NULL;
      }
      // If dimension is different, log a warning.
      if ($dim && (int) $dim !== (int) $dimension) {
        watchdog('ai_search_pgvector', 'Existing table @t vector dimension (@d1) differs from configured (@d2).', ['@t' => $qualified, '@d1' => $dim, '@d2' => $dimension], WATCHDOG_WARNING);
      }
    }

    // Create IVFFLAT index if missing.
    $metric = !empty($this->options['metric_type']) ? $this->options['metric_type'] : 'COSINE';
    $opclass = $this->metricOpclass($metric);
    $lists = !empty($this->options['ivfflat_lists']) ? (int) $this->options['ivfflat_lists'] : 100;
    $index_name = $table . '_ivfflat_idx';
    $exists_idx = db_query("SELECT 1 FROM pg_indexes WHERE schemaname = :s AND tablename = :t AND indexname = :i", [':s' => $schema, ':t' => $table, ':i' => $index_name])->fetchField();
    if (!$exists_idx) {
      db_query('CREATE INDEX ' . $index_name . ' ON ' . $qualified . ' USING ivfflat (vector ' . $opclass . ') WITH (lists = ' . (int) $lists . ')');
      // ANALYZE recommended for IVFFLAT.
      db_query('ANALYZE ' . $qualified);
    }
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
}
