/*-------------------------------------------------------------------------
 *
 * gtm.c
 * 
 *	  Module interfacing with GTM 
 *
 *
 *-------------------------------------------------------------------------
 */

#include <sys/types.h>
#include <unistd.h>

#include "gtm/libpq-fe.h"
#include "gtm/gtm_client.h"
#include "access/gtm.h"
#include "access/transam.h"
#include "utils/elog.h"
#include "miscadmin.h"
#include "pgxc/pgxc.h"

/* Configuration variables */
char *GtmHost = "localhost";
int GtmPort = 6666;
int PGXCNodeId = 1;

extern bool FirstSnapshotSet;

static GTM_Conn *conn;

bool
IsGTMConnected()
{
	return conn != NULL;
}

static void
CheckConnection(void)
{
	/* Be sure that a backend does not use a postmaster connection */
	if (IsUnderPostmaster && GTMPQispostmaster(conn) == 1)
	{
		InitGTM();
		return;
	}

	if (GTMPQstatus(conn) != CONNECTION_OK)
		InitGTM();
}

void
InitGTM(void)
{
	/* 256 bytes should be enough */
	char conn_str[256];

	/* If this thread is postmaster itself, it contacts gtm identifying itself */
	if (!IsUnderPostmaster)
	{
		GTM_PGXCNodeType remote_type = PGXC_NODE_DEFAULT;

		if (IS_PGXC_COORDINATOR)
			remote_type = PGXC_NODE_COORDINATOR;
		else if (IS_PGXC_DATANODE)
			remote_type = PGXC_NODE_DATANODE;

		sprintf(conn_str, "host=%s port=%d pgxc_node_id=%d remote_type=%d postmaster=1",
								GtmHost, GtmPort, PGXCNodeId, remote_type);

		/* Log activity of GTM connections */
		elog(DEBUG1, "Postmaster: connection established to GTM with string %s", conn_str);
	}
	else
	{
		sprintf(conn_str, "host=%s port=%d pgxc_node_id=%d", GtmHost, GtmPort, PGXCNodeId);

		/* Log activity of GTM connections */
		if (IsAutoVacuumWorkerProcess())
			elog(DEBUG1, "Autovacuum worker: connection established to GTM with string %s", conn_str);
		else if (IsAutoVacuumLauncherProcess())
			elog(DEBUG1, "Autovacuum launcher: connection established to GTM with string %s", conn_str);
		else
			elog(DEBUG1, "Postmaster child: connection established to GTM with string %s", conn_str);
	}

	conn = PQconnectGTM(conn_str);
	if (GTMPQstatus(conn) != CONNECTION_OK)
	{
		int save_errno = errno;

		ereport(WARNING,
				(errcode(ERRCODE_CONNECTION_EXCEPTION),
				 errmsg("can not connect to GTM: %m")));

		errno = save_errno;

		CloseGTM();
	}
}

void
CloseGTM(void)
{
	GTMPQfinish(conn);
	conn = NULL;

	/* Log activity of GTM connections */
	if (!IsUnderPostmaster)
		elog(DEBUG1, "Postmaster: connection to GTM closed");
	else if (IsAutoVacuumWorkerProcess())
		elog(DEBUG1, "Autovacuum worker: connection to GTM closed");
	else if (IsAutoVacuumLauncherProcess())
		elog(DEBUG1, "Autovacuum launcher: connection to GTM closed");
	else
		elog(DEBUG1, "Postmaster child: connection to GTM closed");
}

GlobalTransactionId
BeginTranGTM(GTM_Timestamp *timestamp)
{
	GlobalTransactionId  xid = InvalidGlobalTransactionId;

	CheckConnection();
	// TODO Isolation level
	if (conn)
		xid =  begin_transaction(conn, GTM_ISOLATION_RC, timestamp);

	/* If something went wrong (timeout), try and reset GTM connection 
	 * and retry. This is safe at the beginning of a transaction.
	 */
	if (!TransactionIdIsValid(xid))
	{
		CloseGTM();
		InitGTM();
		if (conn)
			xid = begin_transaction(conn, GTM_ISOLATION_RC, timestamp);
	}
	return xid;
}

GlobalTransactionId
BeginTranAutovacuumGTM(void)
{
	GlobalTransactionId  xid = InvalidGlobalTransactionId;

	CheckConnection();
	// TODO Isolation level
	if (conn)
		xid =  begin_transaction_autovacuum(conn, GTM_ISOLATION_RC);

	/*
	 * If something went wrong (timeout), try and reset GTM connection and retry.
	 * This is safe at the beginning of a transaction.
	 */
	if (!TransactionIdIsValid(xid))
	{
		CloseGTM();
		InitGTM();
		if (conn)
			xid =  begin_transaction_autovacuum(conn, GTM_ISOLATION_RC);
	}
	return xid;
}

int
CommitTranGTM(GlobalTransactionId gxid)
{
	int ret;

	if (!GlobalTransactionIdIsValid(gxid))
		return 0;
	CheckConnection();
	ret = commit_transaction(conn, gxid);

	/*
	 * If something went wrong (timeout), try and reset GTM connection. 
	 * We will close the transaction locally anyway, and closing GTM will force
	 * it to be closed on GTM.
	 */
	if (ret < 0)
	{
		CloseGTM();
		InitGTM();
	}

	/* Close connection in case commit is done by autovacuum worker or launcher */
	if (IsAutoVacuumWorkerProcess() || IsAutoVacuumLauncherProcess())
		CloseGTM();

	return ret;
}

/*
 * For a prepared transaction, commit the gxid used for PREPARE TRANSACTION
 * and for COMMIT PREPARED.
 */
int
CommitPreparedTranGTM(GlobalTransactionId gxid, GlobalTransactionId prepared_gxid)
{
	int ret = 0;

	if (!GlobalTransactionIdIsValid(gxid) || !GlobalTransactionIdIsValid(prepared_gxid))
		return ret;
	CheckConnection();
	ret = commit_prepared_transaction(conn, gxid, prepared_gxid);

	/*
	 * If something went wrong (timeout), try and reset GTM connection.
	 * We will close the transaction locally anyway, and closing GTM will force
	 * it to be closed on GTM.
	 */

	if (ret < 0)
	{
		CloseGTM();
		InitGTM();
	}
	return ret;
}

int
RollbackTranGTM(GlobalTransactionId gxid)
{
	int ret = -1;

	if (!GlobalTransactionIdIsValid(gxid))
		return 0;
	CheckConnection();

	if (conn)
		ret = abort_transaction(conn, gxid);

	/*
	 * If something went wrong (timeout), try and reset GTM connection. 
	 * We will abort the transaction locally anyway, and closing GTM will force
	 * it to end on GTM.
	 */
	if (ret < 0)
	{
		CloseGTM();
		InitGTM();
	}
	return ret;
}

int
StartPreparedTranGTM(GlobalTransactionId gxid,
					char *gid,
					int datanodecnt,
					PGXC_NodeId datanodes[],
					int coordcnt,
					PGXC_NodeId coordinators[])
{
	int ret = 0;

	if (!GlobalTransactionIdIsValid(gxid))
		return 0;
	CheckConnection();

	ret = start_prepared_transaction(conn, gxid, gid, datanodecnt, datanodes, coordcnt, coordinators);

	/*
	 * If something went wrong (timeout), try and reset GTM connection.
	 * We will abort the transaction locally anyway, and closing GTM will force
	 * it to end on GTM.
	 */
	if (ret < 0)
	{
		CloseGTM();
		InitGTM();
	}

	return ret;
}

int
PrepareTranGTM(GlobalTransactionId gxid)
{
	int ret;

	if (!GlobalTransactionIdIsValid(gxid))
		return 0;
	CheckConnection();
	ret = prepare_transaction(conn, gxid);

	/*
	 * If something went wrong (timeout), try and reset GTM connection. 
	 * We will close the transaction locally anyway, and closing GTM will force
	 * it to be closed on GTM.
	 */
	if (ret < 0)
	{
		CloseGTM();
		InitGTM();
	}
	return ret;
}


int
GetGIDDataGTM(char *gid,
			  GlobalTransactionId *gxid,
			  GlobalTransactionId *prepared_gxid,
			  int *datanodecnt,
			  PGXC_NodeId **datanodes,
			  int *coordcnt,
			  PGXC_NodeId **coordinators)
{
	int ret = 0;

	CheckConnection();
	ret = get_gid_data(conn, GTM_ISOLATION_RC, gid, gxid,
					   prepared_gxid, datanodecnt, datanodes,
					   coordcnt, coordinators);

	/*
	 * If something went wrong (timeout), try and reset GTM connection.
	 * We will abort the transaction locally anyway, and closing GTM will force
	 * it to end on GTM.
	 */
	if (ret < 0)
	{
		CloseGTM();
		InitGTM();
	}

	return ret;
}

GTM_Snapshot
GetSnapshotGTM(GlobalTransactionId gxid, bool canbe_grouped)
{
	GTM_Snapshot ret_snapshot = NULL;
	CheckConnection();
	if (conn)
		ret_snapshot = get_snapshot(conn, gxid, canbe_grouped);
	if (ret_snapshot == NULL)
	{
		CloseGTM();
		InitGTM();
	}
	return ret_snapshot;
}


/*
 * Create a sequence on the GTM.
 */
int
CreateSequenceGTM(char *seqname, GTM_Sequence increment, GTM_Sequence minval,
				  GTM_Sequence maxval, GTM_Sequence startval, bool cycle)
{
	GTM_SequenceKeyData seqkey;
	CheckConnection();
	seqkey.gsk_keylen = strlen(seqname);
	seqkey.gsk_key = seqname;

	return conn ? open_sequence(conn, &seqkey, increment, minval, maxval, startval, cycle) : 0;
}

/*
 * Alter a sequence on the GTM
 */
int
AlterSequenceGTM(char *seqname, GTM_Sequence increment, GTM_Sequence minval,
				 GTM_Sequence maxval, GTM_Sequence startval, GTM_Sequence lastval, bool cycle, bool is_restart)
{
	GTM_SequenceKeyData seqkey;
	CheckConnection();
	seqkey.gsk_keylen = strlen(seqname);
	seqkey.gsk_key = seqname;

	return conn ? alter_sequence(conn, &seqkey, increment, minval, maxval, startval, lastval, cycle, is_restart) : 0;
}

/*
 * get the current sequence value
 */

GTM_Sequence
GetCurrentValGTM(char *seqname)
{
	GTM_Sequence ret = -1;
	GTM_SequenceKeyData seqkey;
	CheckConnection();
	seqkey.gsk_keylen = strlen(seqname);
	seqkey.gsk_key = seqname;

	if (conn)
		ret =  get_current(conn, &seqkey);
	if (ret < 0)
	{
		CloseGTM();
		InitGTM();
	}
	return ret;
}

/*
 * Get the next sequence value
 */
GTM_Sequence
GetNextValGTM(char *seqname)
{
	GTM_Sequence ret = -1;
	GTM_SequenceKeyData seqkey;
	CheckConnection();
	seqkey.gsk_keylen = strlen(seqname);
	seqkey.gsk_key = seqname;

	if (conn)
		ret =  get_next(conn, &seqkey);
	if (ret < 0)
	{
		CloseGTM();
		InitGTM();
	}
	return ret;
}

/*
 * Set values for sequence
 */
int
SetValGTM(char *seqname, GTM_Sequence nextval, bool iscalled)
{
	GTM_SequenceKeyData seqkey;
	CheckConnection();
	seqkey.gsk_keylen = strlen(seqname);
	seqkey.gsk_key = seqname;

	return conn ? set_val(conn, &seqkey, nextval, iscalled) : -1;
}

/*
 * Drop the sequence depending the key type
 *
 * Type of Sequence name use in key;
 *		GTM_SEQ_FULL_NAME, full name of sequence
 *		GTM_SEQ_DB_NAME, DB name part of sequence key
 */
int
DropSequenceGTM(const char *name, GTM_SequenceKeyType type)
{
	GTM_SequenceKeyData seqkey;
	CheckConnection();
	seqkey.gsk_keylen = strlen(name);
	seqkey.gsk_key = name;
	seqkey.gsk_type = type;

	return conn ? close_sequence(conn, &seqkey) : -1;
}

/*
 * Rename the sequence
 */
int
RenameSequenceGTM(char *seqname, const char *newseqname)
{
	GTM_SequenceKeyData seqkey, newseqkey;
	CheckConnection();
	seqkey.gsk_keylen = strlen(seqname);
	seqkey.gsk_key = seqname;
	newseqkey.gsk_keylen = strlen(newseqname);
	newseqkey.gsk_key = (char *) newseqname;

	return conn ? rename_sequence(conn, &seqkey, &newseqkey) : -1;
}

/*
 * Register Given Node
 * Connection for registering is just used once then closed
 */
int
RegisterGTM(GTM_PGXCNodeType type, GTM_PGXCNodePort port, char *datafolder)
{
	int ret;

	CheckConnection();

	if (!conn)
		return EOF;

	ret = node_register(conn, type, port, PGXCNodeId, datafolder);

	/* If something went wrong, retry once */
	if (ret < 0)
	{
		CloseGTM();
		InitGTM();
		if (conn)
			ret = node_register(conn, type, port, PGXCNodeId, datafolder);
	}

	return ret;
}

/*
 * UnRegister Given Node
 * Connection for registering is just used once then closed
 */
int
UnregisterGTM(GTM_PGXCNodeType type)
{
	int ret;

	CheckConnection();

	if (!conn)
		return EOF;

	ret = node_unregister(conn, type, PGXCNodeId);

	/* If something went wrong, retry once */
	if (ret < 0)
	{
		CloseGTM();
		InitGTM();
		if (conn)
			ret = node_unregister(conn, type, PGXCNodeId);
	}

	/*
	 * If node is unregistered cleanly, cut the connection.
	 * and Node shuts down smoothly.
	 */
	CloseGTM();

	return ret;
}