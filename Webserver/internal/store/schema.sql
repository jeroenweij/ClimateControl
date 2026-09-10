-- ClimateControl server schema. SQLite; one file. See
-- MainController-Server-Link-Spec.md §6.

CREATE TABLE IF NOT EXISTS nodes (
    id          INTEGER PRIMARY KEY,          -- bus node id (1..N)
    module      INTEGER NOT NULL DEFAULT 0,
    fw_version  INTEGER NOT NULL DEFAULT 0,
    first_seen  INTEGER NOT NULL,             -- unix seconds
    last_seen   INTEGER NOT NULL,
    state       INTEGER NOT NULL DEFAULT 0,   -- 0=up, per NodePresence
    online      INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS readings (
    ts        INTEGER NOT NULL,               -- unix millis, server clock at receipt
    node_id   INTEGER NOT NULL,
    endpoint  INTEGER NOT NULL,
    raw       BLOB    NOT NULL,
    value_num REAL,
    value_text TEXT
);
CREATE INDEX IF NOT EXISTS readings_series ON readings (node_id, endpoint, ts);

CREATE TABLE IF NOT EXISTS commands (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    ts        INTEGER NOT NULL,
    node_id   INTEGER NOT NULL,
    endpoint  INTEGER NOT NULL,
    operation INTEGER NOT NULL,
    payload   BLOB,
    "user"    TEXT,
    sent_ts   INTEGER,
    ack_ts    INTEGER,
    result    TEXT
);

CREATE TABLE IF NOT EXISTS ota_jobs (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    node_id     INTEGER NOT NULL,
    filename    TEXT NOT NULL,
    size        INTEGER NOT NULL,
    crc32       INTEGER NOT NULL,
    fw_version  INTEGER NOT NULL,
    module      INTEGER NOT NULL,
    started     INTEGER NOT NULL,
    finished    INTEGER,
    state       TEXT NOT NULL DEFAULT 'pending',
    last_offset INTEGER NOT NULL DEFAULT 0,
    error       TEXT,
    image_path  TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS config_overrides (
    node_id  INTEGER NOT NULL,
    endpoint INTEGER NOT NULL,
    value    REAL NOT NULL,
    "user"   TEXT,
    ts       INTEGER NOT NULL,
    PRIMARY KEY (node_id, endpoint)
);

CREATE TABLE IF NOT EXISTS map_floors (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    name       TEXT NOT NULL,
    image_path TEXT NOT NULL,
    width_px   INTEGER NOT NULL,
    height_px  INTEGER NOT NULL,
    sort       INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS map_placements (
    node_id   INTEGER PRIMARY KEY,
    floor_id  INTEGER NOT NULL,
    x_px      INTEGER NOT NULL,
    y_px      INTEGER NOT NULL,
    poly_json TEXT
);
