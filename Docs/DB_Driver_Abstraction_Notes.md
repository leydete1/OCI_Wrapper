# DB Driver Abstraction — Notes for Next Thread

**Context:** Core OCI/Oracle backend is now complete (plain connect+select in
January → full CRUD → Independent DDL Module with live execute stage,
finished 08-Sep). Next step: strip Oracle-specific access code out of the
CRUD modules (connect, select, insert, update, delete, DDL) into dedicated
driver modules, selected at runtime from `db_type` in `config.ini`. Oracle
is driver #1; SQL Server would be driver #2, built the same way once the
pattern is proven.

Proof of concept: Connect + Select.

---

## 1. "Map" → probably a vtable, not a lookup table

Only one `db_type` is active per running process, set once at startup. A
runtime map/dictionary is more machinery than needed. A single struct of
function pointers — a `db_driver_t` — populated once from `config.ini` and
called through uniformly everywhere is simpler and just as extensible:

```c
typedef struct {
    int  (*connect)(...);
    void (*disconnect)(...);
    int  (*execute_select)(...);
    int  (*execute_insert)(...);
    int  (*execute_ddl)(...);
    int  (*begin_tx)(...);
    int  (*commit_tx)(...);
    int  (*rollback_tx)(...);
    /* ... */
} db_driver_t;
```

## 2. The interface boundary is the real design problem

The hard part isn't the map — it's deciding exactly what crosses the line
between "core execution" (parsing, validation, dispatch, request/response
shaping — stays generic) and "vendor-specific" (goes in the Oracle driver).
Minimum surface to define up front:

- connect / disconnect / connection pooling
- execute-with-binds (parameterized statements)
- fetch / resultset + column metadata
- transaction begin / commit / rollback
- DDL execution (the 7th capability, just added — needs to fit this same
  interface, including the auto-commit / no-transaction quirk that's
  Oracle-specific but the *concept* of "this op can't be transactional"
  should probably be expressible generically)

Get this boundary right in the PoC and a future SQL Server driver inherits
it cleanly. Get it wrong and the line has to be redrawn mid-migration.

**Acid test for every function considered for the vtable:** would this same
signature make sense for a SQL Server/ODBC call? If yes → core interface.
If it only makes sense for Oracle (OCI handle types, OCI-specific modes)
→ stays inside the driver.

## 3. `oci_context_t` is the biggest blast radius

It's threaded through nearly every module already built — workers, session
cache, the DDL modules, dispatcher. Likely needs to become (or be wrapped
by) a generic `db_context_t` with an opaque, driver-owned handle inside it
for the Oracle-specific bits (`svchp`, `errhp`, etc.). This is a wide,
mechanical refactor more than a conceptually hard one — worth planning as
its own pass *after* the interface shape is validated by the PoC, not
before.

## 4. Scope the PoC tightly

Connect + Select is a good choice but touches a lot of surface area
(connection pooling, resultset building, CLOB/BLOB handling, metadata
caching). Suggest proving the pattern on simple scalar/VARCHAR selects
first, and explicitly deferring CLOB/BLOB and resultset-cache interaction
until the interface shape is validated — don't try to abstract everything
on the first pass.

## Suggested order of attack

1. Define the `db_driver_t` interface (connect + select only, for the PoC)
2. Move Oracle-specific code for connect/select behind that interface —
   this becomes `driver_oracle.c` or similar
3. Wire `config.ini`'s `db_type` to select and populate the driver struct
   at startup
4. Validate against the existing connect/select test fixtures — same
   external behavior, different internal wiring
5. Only then decide how far `oci_context_t` → `db_context_t` needs to go,
   and whether to extend the PoC to insert/update/delete/DDL before or
   after that refactor
