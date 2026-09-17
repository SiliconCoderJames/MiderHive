-- MiderHive 本地多 Agent 协作平台 Schema
-- 协作规则落地：
--   * 内容只追加、不覆盖：knowledge/memory 用版本号 + is_latest 标记，旧版本永不删除。
--   * 所有写操作在 audit_log 留痕（身份 + 时间 + 内容摘要）。
-- 知识向量表按**维度**分表：knowledge_vec_d<dim>（内置嵌入器 384，Agent 自带向量
-- 可为 64..4096 内任意维度）。vec0 每张表维度固定，分表才能让不同模型的向量共存；
-- 表在代码中按需创建，存量旧库的单张 knowledge_vec 由启动迁移自动搬移。

PRAGMA journal_mode = WAL;
PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS settings (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS agents (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    name         TEXT NOT NULL UNIQUE,
    role         TEXT NOT NULL DEFAULT 'member',   -- member | zcode
    api_key_hash TEXT NOT NULL,
    salt         TEXT NOT NULL DEFAULT '',         -- 加盐哈希；空串=旧格式（sha256 明钥）
    status       TEXT NOT NULL DEFAULT 'offline',
    current_task TEXT,
    last_seen_at TEXT,
    created_at   TEXT NOT NULL,
    updated_at   TEXT NOT NULL
);

-- 共享知识库：同一 uuid 的多个版本构成历史；只增不改。
CREATE TABLE IF NOT EXISTS knowledge_entries (
    id                 INTEGER PRIMARY KEY AUTOINCREMENT,
    uuid               TEXT NOT NULL,
    title              TEXT NOT NULL,
    content            TEXT NOT NULL,
    tags_json          TEXT NOT NULL DEFAULT '[]',
    category           TEXT,
    author             TEXT NOT NULL,
    version            INTEGER NOT NULL DEFAULT 1,
    parent_version_id  INTEGER,
    is_latest          INTEGER NOT NULL DEFAULT 1,
    embedding_provider TEXT,
    created_at         TEXT NOT NULL
);

-- 技能库：新技能必须先注册（skills 表存在）才能被调用。
CREATE TABLE IF NOT EXISTS skills (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    name         TEXT NOT NULL UNIQUE,
    display_name TEXT,
    description  TEXT NOT NULL,
    category     TEXT,
    owner_agent  TEXT NOT NULL,
    param_schema TEXT NOT NULL DEFAULT '{}',
    version      INTEGER NOT NULL DEFAULT 1,
    status       TEXT NOT NULL DEFAULT 'active',
    created_at   TEXT NOT NULL,
    updated_at   TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS skill_invocations (
    id             INTEGER PRIMARY KEY AUTOINCREMENT,
    skill_name     TEXT NOT NULL,
    caller_agent   TEXT NOT NULL,
    params         TEXT NOT NULL DEFAULT '{}',
    result_summary TEXT,
    status         TEXT NOT NULL,               -- success | failed
    duration_ms    INTEGER NOT NULL DEFAULT 0,
    reference_id   TEXT,                        -- 协作上下文追溯：发起调用的消息/任务/错误 uuid（弱关联，可空）
    created_at     TEXT NOT NULL
);

-- 用户记忆：section+key 定位一条画像；修改即生成新版本。
CREATE TABLE IF NOT EXISTS memory_entries (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    section    TEXT NOT NULL,
    key        TEXT NOT NULL,
    value      TEXT NOT NULL,
    author     TEXT NOT NULL,
    version    INTEGER NOT NULL DEFAULT 1,
    is_latest  INTEGER NOT NULL DEFAULT 1,
    created_at TEXT NOT NULL
);

-- Agent 异步交流：留言 / 提问 / 指派任务；不要求同时在线。
CREATE TABLE IF NOT EXISTS messages (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    uuid          TEXT NOT NULL UNIQUE,
    kind          TEXT NOT NULL,                -- note | question | task
    sender        TEXT NOT NULL,
    recipient     TEXT,                         -- NULL = 广播给所有 Agent
    subject       TEXT,
    body          TEXT NOT NULL,
    status        TEXT NOT NULL DEFAULT 'unread',
    parent_uuid   TEXT,
    created_at    TEXT NOT NULL
);

-- 错误日志：报错必须记录，不得静默忽略。
CREATE TABLE IF NOT EXISTS errors (
    id               INTEGER PRIMARY KEY AUTOINCREMENT,
    uuid             TEXT NOT NULL UNIQUE,
    reporter         TEXT NOT NULL,
    severity         TEXT NOT NULL DEFAULT 'error',
    source           TEXT,
    title            TEXT NOT NULL,
    detail           TEXT NOT NULL,
    stack_trace      TEXT,
    status           TEXT NOT NULL DEFAULT 'open',
    resolution_notes TEXT,
    resolved_by      TEXT,
    created_at       TEXT NOT NULL,
    resolved_at      TEXT
);

-- 操作日志：身份 + 时间 + 动作 + 对象 + 内容摘要，完整可追溯。
CREATE TABLE IF NOT EXISTS audit_log (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    actor      TEXT NOT NULL,
    action     TEXT NOT NULL,
    target     TEXT,
    detail     TEXT NOT NULL DEFAULT '{}',
    created_at TEXT NOT NULL
);

-- Token 用量：每次调用一条，按自然周（周一 UTC 起）聚合。
-- idempotency_key 非空时全局唯一，保证同一任务重复上报不重复扣减。
-- 注意：idx_usage_idem 部分唯一索引由启动迁移创建（存量旧库先 ALTER 加列）。
CREATE TABLE IF NOT EXISTS token_usage (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    agent           TEXT NOT NULL,
    week_start      TEXT NOT NULL,
    tokens_in       INTEGER NOT NULL DEFAULT 0,
    tokens_out      INTEGER NOT NULL DEFAULT 0,
    call_type       TEXT,
    model           TEXT NOT NULL DEFAULT '',
    reference_id    TEXT,
    idempotency_key TEXT,
    created_at      TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_knowledge_uuid      ON knowledge_entries(uuid, version);
CREATE INDEX IF NOT EXISTS idx_knowledge_latest    ON knowledge_entries(is_latest, created_at);
CREATE INDEX IF NOT EXISTS idx_knowledge_category  ON knowledge_entries(category);

-- 关键词检索的全文索引：外部内容表（正文只存 knowledge_entries，FTS 只存倒排）。
-- trigram 分词让中英文**子串**都能命中（>=3 个码点；更短查询在代码层回退 LIKE）。
-- 索引覆盖全部版本、检索时用 is_latest=1 过滤——换来触发器只需对称的插入/删除。
CREATE VIRTUAL TABLE IF NOT EXISTS knowledge_fts USING fts5(
    title, content,
    content='knowledge_entries', content_rowid='id',
    tokenize='trigram'
);
CREATE TRIGGER IF NOT EXISTS knowledge_fts_ai AFTER INSERT ON knowledge_entries BEGIN
    INSERT INTO knowledge_fts(rowid, title, content) VALUES (NEW.id, NEW.title, NEW.content);
END;
CREATE TRIGGER IF NOT EXISTS knowledge_fts_au AFTER UPDATE OF title, content ON knowledge_entries BEGIN
    INSERT INTO knowledge_fts(knowledge_fts, rowid, title, content)
    VALUES ('delete', OLD.id, OLD.title, OLD.content);
    INSERT INTO knowledge_fts(rowid, title, content) VALUES (NEW.id, NEW.title, NEW.content);
END;
CREATE TRIGGER IF NOT EXISTS knowledge_fts_ad AFTER DELETE ON knowledge_entries BEGIN
    INSERT INTO knowledge_fts(knowledge_fts, rowid, title, content)
    VALUES ('delete', OLD.id, OLD.title, OLD.content);
END;
CREATE INDEX IF NOT EXISTS idx_memory_latest       ON memory_entries(section, key, is_latest);
CREATE INDEX IF NOT EXISTS idx_messages_recipient  ON messages(recipient, status);
CREATE INDEX IF NOT EXISTS idx_messages_kind       ON messages(kind, created_at);
CREATE INDEX IF NOT EXISTS idx_errors_status       ON errors(status, severity);
CREATE INDEX IF NOT EXISTS idx_audit_actor         ON audit_log(actor, created_at);
CREATE INDEX IF NOT EXISTS idx_audit_action        ON audit_log(action, created_at);
CREATE INDEX IF NOT EXISTS idx_audit_time          ON audit_log(created_at);
CREATE INDEX IF NOT EXISTS idx_usage_week          ON token_usage(week_start, agent);
CREATE INDEX IF NOT EXISTS idx_invocations_skill   ON skill_invocations(skill_name, created_at);
CREATE INDEX IF NOT EXISTS idx_invocations_ref     ON skill_invocations(reference_id);
