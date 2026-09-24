#ifndef METRICS_WRITER_H
#define METRICS_WRITER_H

/* ======================================================================
 * metrics_writer.h
 *
 * Metrics refactor (closure item 5), Stage 2 (2026-08-09). Makes
 * metrics recording fire-and-forget from the request path's own point
 * of view - the real motivation being a genuine production story: a
 * logging module whose own DB write blocked ended up taking down an
 * entire CRM website, because "just record what happened" was allowed
 * to sit on the same critical path as the business transaction it was
 * describing. Nothing here should ever be able to do that.
 *
 * Two independent destinations, two independent queues, two
 * independent dedicated threads - built on generic_queue.h (see that
 * module's own doc comment on why this is its third use in this
 * project). If the DB destination ever gets slow or contended, file
 * logging keeps working completely unaffected, and vice versa -
 * that's the whole point of keeping them apart rather than one thread
 * serving both.
 *
 *   File destination: one row per record, unbatched - cheap, local
 *   I/O, no round-trip cost worth batching for. Existing metrics_write()
 *   (metrics.h) is reused unchanged for the actual write.
 *
 *   DB destination: batched. Flushes whichever comes first -
 *   metrics_per_write records accumulated, or metrics_max_insert_
 *   delay_ms elapsed since the oldest still-unflushed record - via
 *   generic_queue_dequeue_timed(). A quiet period never leaves metrics
 *   sitting unwritten indefinitely just because the batch never filled.
 *   Borrows its own independent pooled session at startup, exactly
 *   like every other dedicated thread in this project (File Consumer,
 *   Session Manager) - flagged deliberately, since this exact gap (a
 *   new thread given no session of its own) has bitten this project
 *   before. The actual bulk INSERT is a Stage 3 concern (the table
 *   doesn't exist yet) - this stage's DB thread batches correctly and
 *   logs what it would have written; see metrics_writer.c's own note
 *   at the insert call site.
 *
 * Both destinations independently toggleable (metrics_file_enabled,
 * metrics_db_enabled in config.ini) - either, both, or neither. With
 * neither enabled, metrics_finalise_and_enqueue() still costs almost
 * nothing (one finalise() call, no queue ever created) - not a
 * silently-expensive no-op.
 * ====================================================================== */

#include "OCI_Connection.h"   /* oci_context_t */
#include "metrics.h"          /* metrics_record_t */
#include "ini_reader.h"       /* app_config_t */
#include "logger.h"           /* logger_t */

typedef struct metrics_writer metrics_writer_t;   /* opaque */

/*
 * metrics_writer_start()
 *
 * metrics_base_ctx must already be fully set up (connected, loggers
 * initialised) - the DB thread (if metrics_db_enabled) borrows its own
 * session from the same pool metrics_base_ctx is attached to, exactly
 * like every other dedicated thread already does. Response to closure
 * proposal (13 Aug 2026): this is now the metrics DB's OWN independent
 * pool, not the business connection pool - callers should pass the
 * dedicated metrics oci_context_t connected via its own
 * OCI_Connect_pool() call, not the business ctx. file_logger is the
 * already-open metrics CSV logger (unchanged from before this stage -
 * see metrics_write()'s own existing contract) - used EXCLUSIVELY for
 * actual CSV data rows, never for anything else. writer_logger is a
 * genuinely separate, dedicated logger (Stage 2 follow-up, 2026-08-09)
 * for these threads' own operational/lifecycle messages ("thread
 * started", "flushing batch of N") - real bug this fixes: those
 * messages used to go through file_logger too, corrupting the CSV's
 * row structure with interleaved log lines wherever one landed
 * between data rows. Same one-logger-per-subsystem separation already
 * used throughout this project (File Consumer, Session Manager each
 * have their own dedicated logger, distinct from any data they touch).
 *
 * Either or both queues/threads are simply not created if their own
 * config flag is off - metrics_finalise_and_enqueue() checks each
 * queue for NULL before ever trying to use it.
 *
 * Returns NULL only on a genuine allocation failure - a disabled
 * destination is not a failure, it's the requested configuration.
 */
metrics_writer_t *metrics_writer_start(oci_context_t *metrics_base_ctx,
                                        app_config_t  *config,
                                        logger_t      *file_logger,
                                        logger_t      *writer_logger);

/*
 * metrics_finalise_and_enqueue()
 *
 * Replaces the old metrics_finalise()+metrics_write() pair used
 * throughout this project. Calls metrics_finalise() itself (unchanged -
 * still synchronous, still cheap, no I/O), then hands off to whichever
 * destination(s) are enabled. Genuinely non-blocking: enqueue never
 * waits for room on either queue, and a full queue just drops that
 * one record for that one destination (logged, not fatal) rather than
 * blocking the caller - the same "drop rather than block" policy
 * already established for session touches.
 *
 * m is the caller's own metrics_record_t (typically stack-local, built
 * up over the life of one request) - NOT retained after this call
 * returns. Ownership of m's own three heap-string fields
 * (input_file_name/input_request/output_response, if the caller set
 * them via strdup() per metrics.h's own doc comment) transfers to this
 * function exactly as it used to transfer to metrics_write() - the
 * caller does not need to free them either way. Internally, this
 * function makes its own independent deep copy for each enabled
 * destination (each with its own independently-strdup()'d heap
 * strings, never a shared pointer) before enqueueing, specifically so
 * two destination threads can never race to free the same pointer -
 * see metrics_writer.c's own doc comment on why a naive shallow struct
 * copy would be a real double-free risk here.
 *
 * Safe to call with writer == NULL (e.g. if metrics_writer_start()
 * itself failed) - just calls metrics_finalise() and returns, metrics
 * silently not persisted anywhere that run. metrics_logger is used
 * only to report a dropped record (queue full) - matches
 * metrics_write()'s own existing logger_t parameter convention, so
 * call sites can pass the exact same ctx->metrics_logger they already
 * have on hand from the old two-call pattern.
 */
void metrics_finalise_and_enqueue(metrics_writer_t *writer,
                                   logger_t          *metrics_logger,
                                   metrics_record_t *m);

/*
 * metrics_writer_stop_and_join()
 *
 * Same "signal shutdown, don't abandon queued work" guarantee as every
 * other dedicated-thread module in this project - each queue is fully
 * drained (file: writes whatever's left; DB: flushes whatever's left,
 * even if it's short of metrics_per_write) before its own thread
 * exits. Safe to call with NULL.
 */
void metrics_writer_stop_and_join(metrics_writer_t *writer);

#endif /* METRICS_WRITER_H */
