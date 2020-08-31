/*
 * src/test/modules/test_libpq/testlibpqbatch.c
 *
 *
 * testlibpqbatch.c
 *		Test of batch execution functionality
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/types.h>
#include "c.h"
#include "libpq-fe.h"
#include "portability/instr_time.h"

#define EXPECT(condition, ...) \
	if (0 == (condition)) \
	{ \
		fprintf(stderr, __VA_ARGS__); \
		goto fail; \
	}
   

static void exit_nicely(PGconn *conn);
static void simple_batch(PGconn *conn);
static void test_disallowed_in_batch(PGconn *conn);
static void batch_insert_pipelined(PGconn *conn, int n_rows);
static void batch_insert_sequential(PGconn *conn, int n_rows);
static void batch_insert_copy(PGconn *conn, int n_rows);
static void test_batch_abort(PGconn *conn);
static void test_singlerowmode(PGconn *conn);
static const Oid INT4OID = 23;

static const char *const drop_table_sql
= "DROP TABLE IF EXISTS batch_demo";
static const char *const create_table_sql
= "CREATE UNLOGGED TABLE batch_demo(id serial primary key, itemno integer);";
static const char *const insert_sql
= "INSERT INTO batch_demo(itemno) VALUES ($1);";

/* max char length of an int32, plus sign and null terminator */
#define MAXINTLEN 12

static void
exit_nicely(PGconn *conn)
{
	PQfinish(conn);
	exit(1);
}

static void
simple_batch(PGconn *conn)
{
	PGresult   *res = NULL;
	const char *dummy_params[1] = {"1"};
	Oid			dummy_param_oids[1] = {INT4OID};

	fprintf(stderr, "simple batch... ");

	/*
	 * Enter batch mode and dispatch a set of operations, which we'll then
	 * process the results of as they come in.
	 *
	 * For a simple case we should be able to do this without interim
	 * processing of results since our out buffer will give us enough slush to
	 * work with and we won't block on sending. So blocking mode is fine.
	 */
	EXPECT(!PQisnonblocking(conn), "Expected blocking connection mode\n");

	EXPECT(PQenterBatchMode(conn), "failed to enter batch mode: %s\n", PQerrorMessage(conn));

	EXPECT(PQsendQueryParams(conn, "SELECT $1", 1, dummy_param_oids,
				            		   dummy_params, NULL, NULL, 0),
				"dispatching SELECT failed: %s\n", PQerrorMessage(conn));

	EXPECT(!PQexitBatchMode(conn), "exiting batch mode with work in progress should fail, but succeeded\n");

	EXPECT(PQbatchSendQueue(conn), "Ending a batch failed: %s\n", PQerrorMessage(conn));

	/*
	 * in batch mode we have to ask for the first result to be processed;
	 * until we do PQgetResult will return null:
	 */
	EXPECT(PQgetResult(conn) == NULL, "PQgetResult returned something in a batch before first PQbatchProcessQueue() call\n");
	EXPECT(PQbatchProcessQueue(conn), "PQbatchProcessQueue() failed at first batch entry: %s\n", PQerrorMessage(conn));

	/* We can't PQbatchProcessQueue when there might still be pending results */
	EXPECT(!PQbatchProcessQueue(conn), "PQbatchProcessQueue() should've failed with pending results: %s\n", PQerrorMessage(conn));

	res = PQgetResult(conn);
	EXPECT(res != NULL, "PQgetResult returned null when there's a batch item: %s\n", PQerrorMessage(conn));

	EXPECT(PQresultStatus(res) == PGRES_TUPLES_OK, "Unexpected result code %s from first batch item\n", PQresStatus(PQresultStatus(res)));

	PQclear(res);
	res = NULL;

	EXPECT(PQgetResult(conn) == NULL, "PQgetResult returned something extra after first result before PQbatchProcessQueue() call\n");

	/*
	 * Even though we've processed the result there's still a sync to come and
	 * we can't exit batch mode yet
	 */
	EXPECT(!PQexitBatchMode(conn), "exiting batch mode after query but before sync succeeded incorrectly\n");

	/* should now get an explicit sync result */
	EXPECT(PQbatchProcessQueue(conn), "PQbatchProcessQueue() failed at sync after first batch entry: %s\n", PQerrorMessage(conn));

	res = PQgetResult(conn);
	EXPECT(res != NULL, "PQgetResult returned null when sync result expected: %s\n", PQerrorMessage(conn));

	EXPECT(PQresultStatus(res) == PGRES_BATCH_END, "Unexpected result code %s instead of sync result, error: %s\n", PQresStatus(PQresultStatus(res)), PQerrorMessage(conn))

	PQclear(res);
	res = NULL;

	EXPECT(PQgetResult(conn) == NULL, "PQgetResult returned something extra after end batch call\n");

	/* We're still in a batch... */
	EXPECT(PQbatchStatus(conn) != PQBATCH_MODE_OFF, "Fell out of batch mode somehow\n");

	/* until we end it, which we can safely do now */
	EXPECT(PQexitBatchMode(conn), "attempt to exit batch mode failed when it should've succeeded: %s\n", PQerrorMessage(conn));

	EXPECT(PQbatchStatus(conn) == PQBATCH_MODE_OFF, "exiting batch mode didn't seem to work\n");

	fprintf(stderr, "ok\n");

	return;

fail:
	PQclear(res);
	exit_nicely(conn);
}

static void
test_disallowed_in_batch(PGconn *conn)
{
	PGresult   *res = NULL;

	fprintf(stderr, "test error cases... ");

	EXPECT(!PQisnonblocking(conn), "Expected blocking connection mode: %u\n", __LINE__);

	EXPECT(PQenterBatchMode(conn), "Unable to enter batch mode\n");

	EXPECT(PQbatchStatus(conn) != PQBATCH_MODE_OFF, "Batch mode not activated properly\n");

	/* PQexec should fail in batch mode */
	res = PQexec(conn, "SELECT 1");
	EXPECT(PQresultStatus(res) == PGRES_FATAL_ERROR, "PQexec should fail in batch mode but succeeded\n");

	/* So should PQsendQuery */
	EXPECT(PQsendQuery(conn, "SELECT 1") == 0, "PQsendQuery should fail in batch mode but succeeded\n");

	/* Entering batch mode when already in batch mode is OK */
	EXPECT(PQenterBatchMode(conn), "re-entering batch mode should be a no-op but failed\n");

	EXPECT(!PQisBusy(conn), "PQisBusy should return false when idle in batch, returned true\n");

	/* ok, back to normal command mode */
	EXPECT(PQexitBatchMode(conn), "couldn't exit idle empty batch mode\n");

	EXPECT(PQbatchStatus(conn) == PQBATCH_MODE_OFF, "Batch mode not terminated properly\n");

	/* exiting batch mode when not in batch mode should be a no-op */
	EXPECT(PQexitBatchMode(conn), "batch mode exit when not in batch mode should succeed but failed\n");

	/* can now PQexec again */
	res = PQexec(conn, "SELECT 1");
	EXPECT(PQresultStatus(res) == PGRES_TUPLES_OK, "PQexec should succeed after exiting batch mode but failed with: %s\n", PQerrorMessage(conn));

	fprintf(stderr, "ok\n");

	return;

fail:
	PQclear(res);
	exit_nicely(conn);
}

static void
multi_batch(PGconn *conn)
{
	PGresult   *res = NULL;
	const char *dummy_params[1] = {"1"};
	Oid			dummy_param_oids[1] = {INT4OID};

	fprintf(stderr, "multi batch... ");

	/*
	 * Queue up a couple of small batches and process each without returning
	 * to command mode first.
	 */
	EXPECT(PQenterBatchMode(conn), "failed to enter batch mode: %s\n", PQerrorMessage(conn));

	EXPECT(PQsendQueryParams(conn, "SELECT $1", 1, dummy_param_oids,
						   dummy_params, NULL, NULL, 0), "dispatching first SELECT failed: %s\n", PQerrorMessage(conn));

	EXPECT(PQbatchSendQueue(conn), "Ending first batch failed: %s\n", PQerrorMessage(conn));

	EXPECT(PQsendQueryParams(conn, "SELECT $1", 1, dummy_param_oids,
						   dummy_params, NULL, NULL, 0), "dispatching second SELECT failed: %s\n", PQerrorMessage(conn));

	EXPECT(PQbatchSendQueue(conn), "Ending second batch failed: %s\n", PQerrorMessage(conn));

	/* OK, start processing the batch results */
	EXPECT(PQgetResult(conn) == NULL, "PQgetResult returned something in a batch before first PQbatchProcessQueue() call\n");

	EXPECT(PQbatchProcessQueue(conn), "PQbatchProcessQueue() failed at first batch entry: %s\n", PQerrorMessage(conn));

	res = PQgetResult(conn);
	EXPECT(res != NULL, "PQgetResult returned null when there's a batch item: %s\n", PQerrorMessage(conn));

	EXPECT(PQresultStatus(res) == PGRES_TUPLES_OK, "Unexpected result code %s from first batch item\n", PQresStatus(PQresultStatus(res)));
	PQclear(res);
	res = NULL;

	EXPECT(PQgetResult(conn) == NULL, "PQgetResult returned something extra after first result before PQbatchProcessQueue() call\n");

	EXPECT(!PQexitBatchMode(conn), "exiting batch mode after query but before sync succeeded incorrectly\n");

	EXPECT(PQbatchProcessQueue(conn), "PQbatchProcessQueue() failed at sync after first batch entry: %s\n", PQerrorMessage(conn));

	res = PQgetResult(conn);
	EXPECT(res != NULL, "PQgetResult returned null when sync result expected: %s\n", PQerrorMessage(conn));

	EXPECT(PQresultStatus(res) == PGRES_BATCH_END, "Unexpected result code %s instead of sync result, error: %s (line %u)\n", PQresStatus(PQresultStatus(res)), PQerrorMessage(conn), __LINE__);

	PQclear(res);
	res = NULL;

	/* second batch */
	EXPECT(PQbatchProcessQueue(conn), "PQbatchProcessQueue() failed at second batch entry: %s\n", PQerrorMessage(conn));

	res = PQgetResult(conn);
	EXPECT(res != NULL, "PQgetResult returned null when there's a batch item: %s\n", PQerrorMessage(conn));

	EXPECT(PQresultStatus(res) == PGRES_TUPLES_OK, "Unexpected result code %s from second batch item\n",				PQresStatus(PQresultStatus(res)));

	EXPECT(PQbatchProcessQueue(conn), "PQbatchProcessQueue() failed at second batch sync: %s\n", PQerrorMessage(conn));

	res = PQgetResult(conn);
	EXPECT(res != NULL, "PQgetResult returned null when there's a batch item: %s\n", PQerrorMessage(conn));

	EXPECT(PQresultStatus(res) == PGRES_BATCH_END, "Unexpected result code %s from second end batch\n",				PQresStatus(PQresultStatus(res)));

	/* We're still in a batch... */
	EXPECT(PQbatchStatus(conn) != PQBATCH_MODE_OFF, "Fell out of batch mode somehow\n");

	/* until we end it, which we can safely do now */
	EXPECT(PQexitBatchMode(conn), "attempt to exit batch mode failed when it should've succeeded: %s\n", PQerrorMessage(conn));

	EXPECT(PQbatchStatus(conn) == PQBATCH_MODE_OFF, "exiting batch mode didn't seem to work\n");

	fprintf(stderr, "ok\n");

	return;

fail:
	PQclear(res);
	exit_nicely(conn);
}

/*
 * When an operation in a batch fails the rest of the batch is flushed. We
 * still have to get results for each batch item, but the item will just be
 * a PGRES_BATCH_ABORTED code.
 *
 * This intentionally doesn't use a transaction to wrap the batch. You should
 * usually use an xact, but in this case we want to observe the effects of each
 * statement.
 */
static void
test_batch_abort(PGconn *conn)
{
	PGresult   *res = NULL;
	const char *dummy_params[1] = {"1"};
	Oid			dummy_param_oids[1] = {INT4OID};
	int			i;

	fprintf(stderr, "aborted batch... ");

	res = PQexec(conn, drop_table_sql);
	EXPECT(PQresultStatus(res) == PGRES_COMMAND_OK, "dispatching DROP TABLE failed: %s\n", PQerrorMessage(conn));

	res = PQexec(conn, create_table_sql);
	EXPECT(PQresultStatus(res) == PGRES_COMMAND_OK, "dispatching CREATE TABLE failed: %s\n", PQerrorMessage(conn));


	/*
	 * Queue up a couple of small batches and process each without returning
	 * to command mode first. Make sure the second operation in the first
	 * batch ERRORs.
	 */
	EXPECT(PQenterBatchMode(conn), "failed to enter batch mode: %s\n", PQerrorMessage(conn));

	dummy_params[0] = "1";
	EXPECT(PQsendQueryParams(conn, insert_sql, 1, dummy_param_oids,
						   dummy_params, NULL, NULL, 0), "dispatching first INSERT failed: %s\n", PQerrorMessage(conn));

	EXPECT(PQsendQueryParams(conn, "SELECT no_such_function($1)", 1, dummy_param_oids,
						   dummy_params, NULL, NULL, 0), "dispatching error select failed: %s\n", PQerrorMessage(conn));

	dummy_params[0] = "2";
	EXPECT(PQsendQueryParams(conn, insert_sql, 1, dummy_param_oids,
						   dummy_params, NULL, NULL, 0), "dispatching second insert failed: %s\n", PQerrorMessage(conn));

	EXPECT(PQbatchSendQueue(conn), "Ending first batch failed: %s\n", PQerrorMessage(conn));

	dummy_params[0] = "3";
	EXPECT(PQsendQueryParams(conn, insert_sql, 1, dummy_param_oids,
						   dummy_params, NULL, NULL, 0), "dispatching second-batch insert failed: %s\n", PQerrorMessage(conn));

	EXPECT(PQbatchSendQueue(conn), "Ending second batch failed: %s\n", PQerrorMessage(conn));

	/*
	 * OK, start processing the batch results.
	 *
	 * We should get a tuples-ok for the first query, a fatal error, a batch
	 * aborted message for the second insert, a batch-end, then a command-ok
	 * and a batch-ok for the second batch operation.
	 */
	EXPECT(PQbatchProcessQueue(conn), "PQbatchProcessQueue() failed at first batch entry: %s\n", PQerrorMessage(conn));

	EXPECT(((res = PQgetResult(conn)) != NULL) && PQresultStatus(res) == PGRES_COMMAND_OK,
					"Unexpected result code %s from first batch item, error='%s'\n", 
					res == NULL ? "NULL" : PQresStatus(PQresultStatus(res)), 
					res == NULL ? PQerrorMessage(conn) : PQresultErrorMessage(res));

	PQclear(res);
	res = NULL;

	/* second query, caused error */
	EXPECT(PQbatchProcessQueue(conn), "PQbatchProcessQueue() failed at second batch entry: %s\n", PQerrorMessage(conn));

	EXPECT(((res = PQgetResult(conn)) != NULL) && PQresultStatus(res) == PGRES_FATAL_ERROR, "Unexpected result code from second batch item. Wanted PGRES_FATAL_ERROR, got %s\n",res == NULL ? "NULL" : PQresStatus(PQresultStatus(res)));
	PQclear(res);
	res = NULL;

	/*
	 * batch should now be aborted.
	 *
	 * Note that we could still queue more queries at this point if we wanted;
	 * they'd get added to a new third batch since we've already sent a
	 * second. The aborted flag relates only to the batch being received.
	 */
	EXPECT(PQbatchStatus(conn) == PQBATCH_MODE_ABORTED, "batch should be flagged as aborted but isn't\n");

	/* third query in batch, the second insert */
	EXPECT(PQbatchProcessQueue(conn), "PQbatchProcessQueue() failed at third batch entry: %s\n", PQerrorMessage(conn));

	EXPECT(((res = PQgetResult(conn)) != NULL) && PQresultStatus(res) == PGRES_BATCH_ABORTED, "Unexpected result code from third batch item. Wanted PGRES_BATCH_ABORTED, got %s\n", res == NULL ? "NULL" : PQresStatus(PQresultStatus(res)));
	PQclear(res);
	res = NULL;

	EXPECT(PQbatchStatus(conn) == PQBATCH_MODE_ABORTED, "batch should be flagged as aborted but isn't\n");

	/* We're still in a batch... */
	EXPECT(PQbatchStatus(conn) != PQBATCH_MODE_OFF, "Fell out of batch mode somehow\n");

	/* the batch sync */
	EXPECT(PQbatchProcessQueue(conn), "PQbatchProcessQueue() failed at first batch sync: %s\n", PQerrorMessage(conn));

	/*
	 * The end of a failed batch is still a PGRES_BATCH_END so clients know to
	 * start processing results normally again and can tell the difference
	 * between skipped commands and the sync.
	 */
	EXPECT(((res = PQgetResult(conn)) != NULL) && PQresultStatus(res) == PGRES_BATCH_END, 
				 "Unexpected result code from first batch sync. Wanted PGRES_BATCH_END, got %s\n", 
				 res == NULL ? "NULL" : PQresStatus(PQresultStatus(res)));

	PQclear(res);
	res = NULL;

	EXPECT(PQbatchStatus(conn) != PQBATCH_MODE_ABORTED, "sync should've cleared the aborted flag but didn't\n");

	/* We're still in a batch... */
	EXPECT(PQbatchStatus(conn) != PQBATCH_MODE_OFF, "Fell out of batch mode somehow\n");

	/* the insert from the second batch */
	EXPECT(PQbatchProcessQueue(conn), "PQbatchProcessQueue() failed at first entry in second batch: %s\n", PQerrorMessage(conn));

	EXPECT(((res = PQgetResult(conn)) != NULL) && PQresultStatus(res) == PGRES_COMMAND_OK, "Unexpected result code %s from first item in second batch\n", res == NULL ? "NULL" : PQresStatus(PQresultStatus(res)));
	PQclear(res);
	res = NULL;

	/* the second batch sync */
	EXPECT(PQbatchProcessQueue(conn), "PQbatchProcessQueue() failed at second batch sync: %s\n", PQerrorMessage(conn));

	EXPECT(((res = PQgetResult(conn)) != NULL) && PQresultStatus(res) == PGRES_BATCH_END, "Unexpected result code %s from second batch sync\n", res == NULL ? "NULL" : PQresStatus(PQresultStatus(res)));
	PQclear(res);
	res = NULL;

	/* We're still in a batch... */
	EXPECT(PQbatchStatus(conn) != PQBATCH_MODE_OFF, "Fell out of batch mode somehow\n");

	/* until we end it, which we can safely do now */
	EXPECT(PQexitBatchMode(conn), "attempt to exit batch mode failed when it should've succeeded: %s\n", PQerrorMessage(conn));

	EXPECT(PQbatchStatus(conn) == PQBATCH_MODE_OFF, "exiting batch mode didn't seem to work\n");

	fprintf(stderr, "ok\n");

	/*
	 * Since we fired the batches off without a surrounding xact, the results
	 * should be:
	 *
	 * - Implicit xact started by server around 1st batch - First insert
	 * applied - Second statement aborted xact - Third insert skipped - Sync
	 * rolled back first implicit xact - Implicit xact created by server
	 * around 2nd batch - insert applied from 2nd batch - Sync commits 2nd
	 * xact
	 *
	 * So we should only have the value 3 that we inserted.
	 */
	res = PQexec(conn, "SELECT itemno FROM batch_demo");

	EXPECT(PQresultStatus(res) == PGRES_TUPLES_OK, "Expected tuples, got %s: %s", PQresStatus(PQresultStatus(res)), PQerrorMessage(conn));

	for (i = 0; i < PQntuples(res); i++)
	{
		const char *val = PQgetvalue(res, i, 0);

		EXPECT(strcmp(val, "3") == 0, "expected only insert with value 3, got %s", val);
	}

	EXPECT(PQntuples(res) == 1, "expected 1 result, got %d", PQntuples(res));
	PQclear(res);

	return;

fail:
	PQclear(res);
	exit_nicely(conn);
}


/* State machine enums for batch insert */
typedef enum BatchInsertStep
{
	BI_BEGIN_TX,
	BI_DROP_TABLE,
	BI_CREATE_TABLE,
	BI_PREPARE,
	BI_INSERT_ROWS,
	BI_COMMIT_TX,
	BI_SYNC,
	BI_DONE
}	BatchInsertStep;

static void
batch_insert_pipelined(PGconn *conn, int n_rows)
{
	PGresult   *res = NULL;
	const char *insert_params[1];
	Oid			insert_param_oids[1] = {INT4OID};
	char		insert_param_0[MAXINTLEN];
	BatchInsertStep send_step = BI_BEGIN_TX,
				recv_step = BI_BEGIN_TX;
	int			rows_to_send,
				rows_to_receive;

	insert_params[0] = &insert_param_0[0];

	rows_to_send = rows_to_receive = n_rows;

	/*
	 * Do a batched insert into a table created at the start of the batch
	 */
	EXPECT(PQenterBatchMode(conn), "failed to enter batch mode: %s\n", PQerrorMessage(conn));

	EXPECT(PQsendQueryParams(conn, "BEGIN",
						   0, NULL, NULL, NULL, NULL, 0), "xact start failed: %s\n", PQerrorMessage(conn));

	fprintf(stdout, "sent BEGIN\n");

	send_step = BI_DROP_TABLE;

	EXPECT(PQsendQueryParams(conn, drop_table_sql,
						   0, NULL, NULL, NULL, NULL, 0), "dispatching DROP TABLE failed: %s\n", PQerrorMessage(conn));

	fprintf(stdout, "sent DROP\n");

	send_step = BI_CREATE_TABLE;

	EXPECT(PQsendQueryParams(conn, create_table_sql,
						   0, NULL, NULL, NULL, NULL, 0), "dispatching CREATE TABLE failed: %s\n", PQerrorMessage(conn));

	fprintf(stdout, "sent CREATE\n");

	send_step = BI_PREPARE;

	EXPECT(PQsendPrepare(conn, "my_insert", insert_sql, 1, insert_param_oids), "dispatching PREPARE failed: %s\n", PQerrorMessage(conn));

	fprintf(stdout, "sent PREPARE\n");

	send_step = BI_INSERT_ROWS;

	/*
	 * Now we start inserting. We'll be sending enough data that we could fill
	 * our out buffer, so to avoid deadlocking we need to enter nonblocking
	 * mode and consume input while we send more output. As results of each
	 * query are processed we should pop them to allow processing of the next
	 * query. There's no need to finish the batch before processing results.
	 */
	EXPECT(PQsetnonblocking(conn, 1) == 0, "failed to set nonblocking mode: %s\n", PQerrorMessage(conn));

	while (recv_step != BI_DONE)
	{
		int			sock;
		fd_set		input_mask;
		fd_set		output_mask;

		sock = PQsocket(conn);

		if (sock < 0)
			break;				/* shouldn't happen */

		FD_ZERO(&input_mask);
		FD_SET(sock, &input_mask);
		FD_ZERO(&output_mask);
		FD_SET(sock, &output_mask);

		if (select(sock + 1, &input_mask, &output_mask, NULL, NULL) < 0)
		{
			fprintf(stderr, "select() failed: %s\n", strerror(errno));
			exit_nicely(conn);
		}

		/*
		 * Process any results, so we keep the server's out buffer free
		 * flowing and it can continue to process input
		 */
		if (FD_ISSET(sock, &input_mask))
		{
			PQconsumeInput(conn);

			/* Read until we'd block if we tried to read */
			while (!PQisBusy(conn) && recv_step < BI_DONE)
			{
				const char *cmdtag;
				const char *description = NULL;
				int			status;
				BatchInsertStep next_step;


				res = PQgetResult(conn);

				if (res == NULL)
				{
					/*
					 * No more results from this query, advance to the next
					 * result
					 */
					EXPECT(PQbatchProcessQueue(conn), "Expected next query result but unable to dequeue: %s\n",
								PQerrorMessage(conn));

					fprintf(stdout, "next query!\n");
					continue;
				}

				status = PGRES_COMMAND_OK;
				next_step = recv_step + 1;
				switch (recv_step)
				{
					case BI_BEGIN_TX:
						cmdtag = "BEGIN";
						break;
					case BI_DROP_TABLE:
						cmdtag = "DROP TABLE";
						break;
					case BI_CREATE_TABLE:
						cmdtag = "CREATE TABLE";
						break;
					case BI_PREPARE:
						cmdtag = "";
						description = "PREPARE";
						break;
					case BI_INSERT_ROWS:
						cmdtag = "INSERT";
						rows_to_receive--;
						if (rows_to_receive > 0)
							next_step = BI_INSERT_ROWS;
						break;
					case BI_COMMIT_TX:
						cmdtag = "COMMIT";
						break;
					case BI_SYNC:
						cmdtag = "";
						description = "SYNC";
						status = PGRES_BATCH_END;
						break;
					case BI_DONE:
						/* unreachable */
						abort();
				}
				if (description == NULL)
					description = cmdtag;

				fprintf(stderr, "At state %d (%s) expect tag '%s', result code %s, expect %d more rows, transition to %d\n",
						recv_step, description, cmdtag, PQresStatus(status), rows_to_receive, next_step);

				EXPECT(PQresultStatus(res) == status, "%s reported status %s, expected %s. Error msg is [%s]\n",
							description, PQresStatus(PQresultStatus(res)), PQresStatus(status), PQerrorMessage(conn));

				EXPECT(strncmp(PQcmdStatus(res), cmdtag, strlen(cmdtag)) == 0, "%s expected command tag '%s', got '%s'\n",
							description, cmdtag, PQcmdStatus(res));

				fprintf(stdout, "Got %s OK\n", cmdtag);

				recv_step = next_step;

				PQclear(res);
				res = NULL;
			}
		}

		/* Write more rows and/or the end batch message, if needed */
		if (FD_ISSET(sock, &output_mask))
		{
			PQflush(conn);

			if (send_step == BI_INSERT_ROWS)
			{
				snprintf(&insert_param_0[0], MAXINTLEN, "%d", rows_to_send);
				insert_param_0[MAXINTLEN - 1] = '\0';

				if (PQsendQueryPrepared(conn, "my_insert",
										1, insert_params, NULL, NULL, 0))
				{
					fprintf(stdout, "sent row %d\n", rows_to_send);

					rows_to_send--;
					if (rows_to_send == 0)
						send_step = BI_COMMIT_TX;
				}
				else
				{
					/*
					 * in nonblocking mode, so it's OK for an insert to fail
					 * to send
					 */
					fprintf(stderr, "WARNING: failed to send insert #%d: %s\n",
							rows_to_send, PQerrorMessage(conn));
				}
			}
			else if (send_step == BI_COMMIT_TX)
			{
				if (PQsendQueryParams(conn, "COMMIT",
									  0, NULL, NULL, NULL, NULL, 0))
				{
					fprintf(stdout, "sent COMMIT\n");
					send_step = BI_SYNC;
				}
				else
				{
					fprintf(stderr, "WARNING: failed to send commit: %s\n",
							PQerrorMessage(conn));
				}
			}
			else if (send_step == BI_SYNC)
			{
				if (PQbatchSendQueue(conn))
				{
					fprintf(stdout, "Dispatched end batch message\n");
					send_step = BI_DONE;
				}
				else
				{
					fprintf(stderr, "WARNING: Ending a batch failed: %s\n",
							PQerrorMessage(conn));
				}
			}
		}

	}

	/* We've got the sync message and the batch should be done */
	EXPECT(PQexitBatchMode(conn), "attempt to exit batch mode failed when it should've succeeded: %s\n", PQerrorMessage(conn));

	EXPECT(PQsetnonblocking(conn, 0) == 0, "failed to clear nonblocking mode: %s\n", PQerrorMessage(conn));

	return;

fail:
	PQclear(res);
	exit_nicely(conn);
}


static void
batch_insert_sequential(PGconn *conn, int nrows)
{
	PGresult   *res = NULL;
	const char *insert_params[1];
	Oid			insert_param_oids[1] = {INT4OID};
	char		insert_param_0[MAXINTLEN];

	insert_params[0] = &insert_param_0[0];

	res = PQexec(conn, "BEGIN");
	EXPECT(PQresultStatus(res) == PGRES_COMMAND_OK, "BEGIN failed: %s\n", PQerrorMessage(conn));
	PQclear(res);

	res = PQexec(conn, drop_table_sql);
	EXPECT(PQresultStatus(res) == PGRES_COMMAND_OK, "DROP TABLE failed: %s\n", PQerrorMessage(conn));
	PQclear(res);

	res = PQexec(conn, create_table_sql);
	EXPECT(PQresultStatus(res) == PGRES_COMMAND_OK, "CREATE TABLE failed: %s\n", PQerrorMessage(conn));
	PQclear(res);

	res = PQprepare(conn, "my_insert2", insert_sql, 1, insert_param_oids);
	EXPECT(PQresultStatus(res) == PGRES_COMMAND_OK, "prepare failed: %s\n", PQerrorMessage(conn));
	PQclear(res);

	while (nrows > 0)
	{
		snprintf(&insert_param_0[0], MAXINTLEN, "%d", nrows);
		insert_param_0[MAXINTLEN - 1] = '\0';

		res = PQexecPrepared(conn, "my_insert2",
							 1, insert_params, NULL, NULL, 0);
		EXPECT(PQresultStatus(res) == PGRES_COMMAND_OK, "INSERT failed: %s\n", PQerrorMessage(conn));

		PQclear(res);
		nrows--;
	}

	res = PQexec(conn, "COMMIT");
	EXPECT(PQresultStatus(res) == PGRES_COMMAND_OK, "COMMIT failed: %s\n", PQerrorMessage(conn));
	PQclear(res);

	return;

fail:
	PQclear(res);
	exit_nicely(conn);
}

static void
batch_insert_copy(PGconn *conn, int nrows)
{
	PGresult   *res = NULL;

	res = PQexec(conn, drop_table_sql);
	EXPECT(PQresultStatus(res) == PGRES_COMMAND_OK, "DROP TABLE failed: %s\n", PQerrorMessage(conn));
	PQclear(res);

	res = PQexec(conn, create_table_sql);
	EXPECT(PQresultStatus(res) == PGRES_COMMAND_OK, "CREATE TABLE failed: %s\n", PQerrorMessage(conn));
	PQclear(res);
	res = NULL;

	res = PQexec(conn, "COPY batch_demo(itemno) FROM stdin");
	EXPECT(PQresultStatus(res) == PGRES_COPY_IN, "COPY: %s\n", PQerrorMessage(conn));
	PQclear(res);
	res = NULL;

	while (nrows > 0)
	{
		char		buf[12 + 2];
		int			formatted = snprintf(&buf[0], 12 + 1, "%d\n", nrows);

		EXPECT(formatted < 12 + 1, "Buffer write truncated somehow\n");

		EXPECT(PQputCopyData(conn, buf, formatted) == 1, "Write of COPY data failed: %s\n",
					PQerrorMessage(conn));

		nrows--;
	}

	EXPECT(PQputCopyEnd(conn, NULL) == 1, "Finishing COPY failed: %s",
				PQerrorMessage(conn));

	res = PQgetResult(conn);
	EXPECT(PQresultStatus(res) == PGRES_COMMAND_OK, "COPY finished with %s: %s\n",			
	    	PQresStatus(PQresultStatus(res)),
				PQresultErrorMessage(res));
	PQclear(res);
	res = NULL;

	return;

fail:
	PQclear(res);
	exit_nicely(conn);
}

static void
test_timings(PGconn *conn, int number_of_rows)
{
	instr_time start_time, end_time;

	fprintf(stderr, "inserting %d rows batched then unbatched\n", number_of_rows);

	INSTR_TIME_SET_CURRENT(start_time);
	batch_insert_pipelined(conn, number_of_rows);
	INSTR_TIME_SET_CURRENT(end_time);
	INSTR_TIME_SUBTRACT(end_time, start_time);

	printf("batch insert elapsed:      %.8f ms\n",
		   INSTR_TIME_GET_MILLISEC(end_time));

	INSTR_TIME_SET_CURRENT(start_time);
	batch_insert_sequential(conn, number_of_rows);
	INSTR_TIME_SET_CURRENT(end_time);
	INSTR_TIME_SUBTRACT(end_time, start_time);
	printf("sequential insert elapsed: %.8f ms\n",
		   INSTR_TIME_GET_MILLISEC(end_time));

	INSTR_TIME_SET_CURRENT(start_time);
	batch_insert_copy(conn, number_of_rows);
	INSTR_TIME_SET_CURRENT(end_time);
	INSTR_TIME_SUBTRACT(end_time, start_time);
	printf("COPY elapsed:              %.8f ms\n",
		   INSTR_TIME_GET_MILLISEC(end_time));

	fprintf(stderr, "Done.\n");
}

static void
usage_exit(const char *progname)
{
	fprintf(stderr, "Usage: %s ['connstring' [number_of_rows [test_to_run]]]\n", progname);
	fprintf(stderr, "  tests: all|disallowed_in_batch|simple_batch|multi_batch|batch_abort|timings|singlerowmode\n");
	exit(1);
}

static void
test_singlerowmode(PGconn *conn)
{
	PGresult *res;
	int i,r;

	/* 1 batch, 3 queries in it */
	r = PQenterBatchMode(conn);

	for (i=0; i < 3; i++) {
		r = PQsendQueryParams(conn,
				"SELECT 1",
				0,
				NULL,
				NULL,
				NULL,
				NULL,
				0);
	}
	PQbatchSendQueue(conn);

	i=0;
	while (PQbatchProcessQueue(conn))
	{
		int	isSingleTuple = 0;
		/* Set single row mode for only first 3 SELECT queries */
		if(i < 3)
		{
			r = PQsetSingleRowMode(conn);
			if (r!=1)
			{
				fprintf(stderr, "PQsetSingleRowMode() failed for i=%d\n", i);
			}
		}
		while ((res = PQgetResult(conn)) != NULL)
		{
			ExecStatusType est = PQresultStatus(res);
			fprintf(stderr, "Result status: %d (%s) for i=%d", est, PQresStatus(est), i);
			if (est == PGRES_TUPLES_OK)
			{
				fprintf(stderr,  ", tuples: %d\n", PQntuples(res));
				EXPECT(isSingleTuple, " Expected to follow PGREG_SINGLE_TUPLE, but received PGRES_TUPLES_OK directly instead\n");
				isSingleTuple=0;
			}
			else if (est == PGRES_SINGLE_TUPLE)
			{
				isSingleTuple = 1;
				fprintf(stderr,  ", single tuple: %d\n", PQntuples(res));
			}
			else if (est == PGRES_BATCH_END)
			{
				fprintf(stderr,  ", end of batch reached\n");
			}
			else if (est != PGRES_COMMAND_OK)
			{
				fprintf(stderr,  ", error: %s\n", PQresultErrorMessage(res));
			}
			PQclear(res);
		}
		i++;
	}
	PQexitBatchMode(conn);
	PQclear(res);
	res = NULL;
	return;
fail:
	PQclear(res);
	exit_nicely(conn);

}
int
main(int argc, char **argv)
{
	const char *conninfo;
	PGconn	   *conn;
	int			number_of_rows = 10000;

	int			run_disallowed_in_batch = 1,
				run_simple_batch = 1,
				run_multi_batch = 1,
				run_batch_abort = 1,
				run_timings = 1,
				run_singlerowmode = 1;

	/*
	 * If the user supplies a parameter on the command line, use it as the
	 * conninfo string; otherwise default to setting dbname=postgres and using
	 * environment variables or defaults for all other connection parameters.
	 */
	if (argc > 4)
	{
		usage_exit(argv[0]);
	}
	if (argc > 3)
	{
		if (strcmp(argv[3], "all") != 0)
		{
			run_disallowed_in_batch = 0;
			run_simple_batch = 0;
			run_multi_batch = 0;
			run_batch_abort = 0;
			run_timings = 0;
			run_singlerowmode = 0;
			if (strcmp(argv[3], "disallowed_in_batch") == 0)
				run_disallowed_in_batch = 1;
			else if (strcmp(argv[3], "simple_batch") == 0)
				run_simple_batch = 1;
			else if (strcmp(argv[3], "multi_batch") == 0)
				run_multi_batch = 1;
			else if (strcmp(argv[3], "batch_abort") == 0)
				run_batch_abort = 1;
			else if (strcmp(argv[3], "timings") == 0)
				run_timings = 1;
			else if (strcmp(argv[3], "singlerowmode") == 0)
				run_singlerowmode = 1;
			else
			{
				fprintf(stderr, "%s is not a recognized test name\n", argv[3]);
				usage_exit(argv[0]);
			}
		}
	}
	if (argc > 2)
	{
		errno = 0;
		number_of_rows = strtol(argv[2], NULL, 10);
		if (errno)
		{
			fprintf(stderr, "couldn't parse '%s' as an integer or zero rows supplied: %s", argv[2], strerror(errno));
			usage_exit(argv[0]);
		}
		if (number_of_rows <= 0)
		{
			fprintf(stderr, "number_of_rows must be positive");
			usage_exit(argv[0]);
		}
	}
	if (argc > 1)
	{
		conninfo = argv[1];
	}
	else
	{
		conninfo = "dbname = postgres";
	}

	/* Make a connection to the database */
	conn = PQconnectdb(conninfo);

	/* Check to see that the backend connection was successfully made */
	if (PQstatus(conn) != CONNECTION_OK)
	{
		fprintf(stderr, "Connection to database failed: %s\n",
				PQerrorMessage(conn));
		exit_nicely(conn);
	}

	if (run_disallowed_in_batch)
		test_disallowed_in_batch(conn);

	if (run_simple_batch)
		simple_batch(conn);

	if (run_multi_batch)
		multi_batch(conn);

	if (run_batch_abort)
		test_batch_abort(conn);

	if (run_timings)
		test_timings(conn, number_of_rows);

	if(run_singlerowmode)
		test_singlerowmode(conn);
	/* close the connection to the database and cleanup */
	PQfinish(conn);

	return 0;
}
