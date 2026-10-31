// Database schema.
//
// Section 8 of the design doc gives a PostgreSQL skeleton and then lists, in
// the note after it, the constraints that "must be designed at the service
// layer and in migrations". This header is both halves for the runnable
// backend: the DDL below is the section 8 schema translated to SQLite, with
// the composite tenant-aware foreign keys, the append-only guards and the
// projection tables that the note demands.
//
// migrations/0001_core.sql holds the PostgreSQL 17 form of the same schema.
// The two are kept deliberately close so that the store logic is portable;
// where the engines genuinely differ (bytea versus hex text, jsonb versus
// canonical text, SKIP LOCKED versus a single writer) the difference is
// commented in both files rather than hidden.
#pragma once

#include <string_view>

#include "jrnl/result.hpp"

namespace jrnl {

class Db;

// Bumped whenever the layout changes in a way a previous binary could not
// read. A database written by a newer schema is refused, never guessed at
// (G18, JI-37).
inline constexpr int kSchemaVersion = 1;

extern const std::string_view kSqliteSchema;

// Creates the schema if absent and checks the recorded version. Returns
// SchemaMismatch when the database was written by a different version.
StoreError apply_schema(Db& db, std::string* why);

// Reads the recorded schema version. Returns -1 when the database carries no
// schema at all.
int read_schema_version(Db& db);

}  // namespace jrnl
