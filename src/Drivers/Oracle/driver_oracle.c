/*
 * driver_oracle.c
 *
 * connect()/disconnect() are thin delegating wrappers around the
 * existing OCI_Connect()/OCI_Disconnect() (OCI_Connection.c) - they are
 * NOT reimplemented here, and OCI_Connect()/OCI_Disconnect() themselves
 * are NOT touched or moved out of OCI_Connection.c in this pass.
 *
 * Why delegate instead of relocate:
 *   OCI_Connect()/OCI_Disconnect() have three other direct callers today
 *   (Data_Manager_Bootstrap.c, Level2_Insert_Test.c, OCI_Unit_Test_Module.c
 *   - the last one specifically exercises a standalone, non-pooled
 *   connect/disconnect cycle as a stale-handle test victim). None of
 *   those go through db_driver_t yet. Relocating the function bodies
 *   into this file now would mean either duplicating them or making
 *   three unrelated call sites depend on this new header before the
 *   interface shape is proven - exactly the risk the notes doc (section
 *   2) warns against ("get it wrong and the line has to be redrawn
 *   mid-migration"). Delegating first gives db_driver_t real behavior
 *   to test against those existing fixtures (order-of-attack step 4)
 *   with zero risk to the other three callers, which are completely
 *   unaffected by this file's existence.
 *
 *   Physically moving the OCI_Connect()/OCI_Disconnect() bodies into
 *   this file - and updating the three other callers to go through
 *   db_driver_t instead - is a follow-up, purely mechanical pass once
 *   this delegation has been validated against Level2_Insert_Test.c's
 *   fixture (see Driver_Connect_Test.c).
 *
 * execute_select is intentionally left NULL - not implemented in this
 * pass. See db_driver.h's doc comment for why (execute_query_batch's
 * fetch loop needs to be split from response-building before it can
 * sit behind this interface; that is separate, larger work).
 */

#include "driver_oracle.h"
#include "OCI_Connection.h"

static int oracle_connect(oci_context_t *ctx)
{
    return OCI_Connect(ctx);
}

static void oracle_disconnect(oci_context_t *ctx)
{
    OCI_Disconnect(ctx);
}

static const db_driver_t oracle_driver = {
    .driver_name     = "oracle",
    .connect         = oracle_connect,
    .disconnect      = oracle_disconnect,
    .execute_select  = NULL   /* not wired up in this pass */
};

const db_driver_t *oracle_driver_get(void)
{
    return &oracle_driver;
}
