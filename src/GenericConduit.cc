/* GenericConduit.cc
 *
 * Methods and such for the generic conduit.
 *
 *	Copyright (C) 1999, 2000, Andrew Arensburger.
 *	You may distribute this file under the terms of the Artistic
 *	License, as specified in the README file.
 *
 * $Id: GenericConduit.cc,v 1.21 2000-04-05 05:45:47 arensb Exp $
 */

/* Note on I/O:
 *
 * C++-style I/O,
 *	cout << "foo" << bar << endl;
 * does not lend itself well to internationalization: a phrase like "file
 * <N>" might be translated into French as "le <N>i�me fichier":
 *
 * cout << "file " << n                   << endl;	// English
 * cout << "le "   << n << "i�me fichier" << endl;	// French
 *
 * This example presents an intractable problem: how does one code this
 * print statement so that it can be looked up in a message catalog and
 * written in the user's language?
 *
 * From this point of view, it is much easier to use C-style I/O:
 *
 *	printf(_("file %n\n"), n);
 *
 * Scott Meyers[1] advocates using C++-style I/O for a number of reasons,
 * mainly having to do with flexibility. But since we're not doing anything
 * fancy here--just printing ordinary strings and integers--it's much
 * easier to use C-style printf()s.
 *
 * [1] Meyers, Scott, "Effective C++," Addison-Wesley, 1992
 */
#include "config.h"
#include <stdio.h>		// For perror(), rename(), printf()
#include <stdlib.h>		// For free()
#include <fcntl.h>		// For open()
#include <sys/stat.h>
#include <sys/param.h>		// For MAXPATHLEN
#include <string.h>
#include <errno.h>
#include "GenericConduit.hh"

extern "C" {
#include <unistd.h>

/* Include I18N-related stuff, if necessary */
#if HAVE_LIBINTL
#  include <locale.h>		/* For setlocale() and friends */
#  include <libintl.h>
#endif	/* HAVE_LIBINTL */

#include "pconn/pconn.h"
#include "pdb.h"
#include "coldsync.h"
#include "archive.h"
}

/* Convenience functions */
static inline bool EXPUNGED(const struct pdb_record *r)
{ return (r->attributes & PDB_REC_EXPUNGED) != 0; }

static inline bool DIRTY(const struct pdb_record *r)
{ return (r->attributes & PDB_REC_DIRTY) != 0; }

static inline bool DELETED(const struct pdb_record *r)
{ return (r->attributes & PDB_REC_DELETED) != 0; }

static inline bool ARCHIVE(const struct pdb_record *r)
{ return (r->attributes & PDB_REC_ARCHIVE) != 0; }

static inline bool PRIVATE(const struct pdb_record *r)
{ return (r->attributes & PDB_REC_PRIVATE) != 0; }

int
run_GenericConduit(
	struct PConnection *pconn,
	struct Palm *palm,
	struct dlp_dbinfo *dbinfo)
{
	GenericConduit gc(pconn, palm, dbinfo);

	return gc.run();
}

/* GenericConduit::GenericConduit
 * Constructor
 */
GenericConduit::GenericConduit(
	struct PConnection *pconn,
	struct Palm *palm,
	struct dlp_dbinfo *dbinfo) :
	_pconn(pconn),
	_palm(palm),
	_dbinfo(dbinfo),
	_localdb(0),
	_remotedb(0),
	_archfd(-1)		// No archive file yet
{}

/* GenericConduit::~GenericConduit
 * Destructor.
 */
GenericConduit::~GenericConduit()
{
	/* _pconn, _palm and _dbinfo are inherited from the function
	 * that called the constructor, so don't free them.
	 */

	if (_localdb != 0)
		free_pdb(_localdb);
	if (_remotedb != 0)
		free_pdb(_remotedb);
}

/* XXX - If syncing a ROM database, bear in mind that you can't upload
 * records.
 */
/* GenericConduit::run
 * Do the sync. Actually, all this method does is to figure out which
 * method needs to do the syncing, and calls it.
 * If the backup file doesn't exist, then this is evidently the first
 * time that this database has been synced, so run FirstSync().
 * Otherwise, call either FastSync or SlowSync(), depending on whether
 * the last sync was with this machine or some other one.
 */
int
GenericConduit::run()
{
	int err;

	/* See if it's a ROM database. If so, just ignore it, since it
	 * can't be modified and hence need not be synced.
	 */
	if (DBINFO_ISROM(_dbinfo) && !global_opts.check_ROM)
	{
		SYNC_TRACE(3)
			fprintf(stderr, "\"%s\" is a ROM database. "
				"ignoring it\n",
				_dbinfo->name);
		add_to_log(_("ROM\n")); 
		return 0; 
	}
	SYNC_TRACE(4)
		fprintf(stderr, "\"%s\" is not a ROM database, or else "
			"I'm not ignoring it.\n",
			_dbinfo->name);

	/* Resource databases are entirely different beasts, so don't
	 * sync them. This function should never be called for a
	 * resource database, though, so this test falls into the
	 * "this should never happen" category.
	 */
	if (DBINFO_ISRSRC(_dbinfo))
	{
		fprintf(stderr, _("I don't deal with resource databases.\n"));
		add_to_log(_("Not synced\n"));
		return -1;
	}

	/* Read the backup file, and put the local database in _localdb */
	err = this->read_backup();
	if (err < 0)
	{
		fprintf(stderr, _("%s error: Can't read "
				  "%s backup file.\n"),
			"GenericConduit", _dbinfo->name);
		return -1;
	}

	this->open_archive();		// Initialize the archive file

	if (_localdb == 0)
	{
		/* Failed to read the backup file, but it wasn't an error.
		 * Hence, it's because the backup file doesn't exist, so do
		 * a FirstSync().
		 */
		err = this->FirstSync();
	} else if (global_opts.force_slow || need_slow_sync)
	{
		SYNC_TRACE(3)
			fprintf(stderr, "Doing a slow sync\n");
		err = this->SlowSync();
	} else {
		SYNC_TRACE(3)
			fprintf(stderr, "Doing a fast sync\n");
		err = this->FastSync(); 
	}

	this->close_archive();		// Close the archive file
	return err;
}

/* GenericConduit::FirstSync
 * This function gets called the first time a database is synced. It
 * is responsible for creating the backup file (and any other
 * initialization that a conduit might wish to do).
 * For the generic conduit, just download the entire database, archive
 * any records that need to be archived, and write everything else to
 * the backup file.
 */
int
GenericConduit::FirstSync()
{
	int err;

	/* Tell the Palm we're beginning a new sync. */
	ubyte dbh;		// Database handle

	add_to_log(_dbinfo->name);
	add_to_log(_(" (1st) - "));

	err = DlpOpenConduit(_pconn);
	if (err != DLPSTAT_NOERR)
	{
		SYNC_TRACE(4)
			fprintf(stderr, "DlpOpenConduit() returned %d\n",
				err);
		add_to_log(_("Error\n"));
		return -1; 
	}

	/* Open the database for reading */
	err = DlpOpenDB(_pconn,
			CARD0,
			_dbinfo->name,
			DLPCMD_MODE_READ |
			(_dbinfo->db_flags & DLPCMD_DBFLAG_OPEN ?
			 0 :
			 DLPCMD_MODE_WRITE) |
			DLPCMD_MODE_SECRET,
			/* "Secret" records aren't actually secret.
			 * They're actually just the ones marked
			 * private, and they're not at all secret.
			 */
			&dbh);
	switch (err)
	{
	    case DLPSTAT_NOERR:
		/* Things are fine */
		break;
	    case DLPSTAT_NOTFOUND:
	    case DLPSTAT_TOOMANYOPEN:
	    case DLPSTAT_CANTOPEN:
		/* Can't complete this particular operation, but
		 * it's not a show-stopper. The sync can go on.
		 */
		fprintf(stderr, _("%s: Can't open \"%s\": %d\n"),
			"GenericConduit::FirstSync",
			_dbinfo->name,
			err);
		add_to_log(_("Error\n"));
		return -1;
	    default:
		/* Some other error, which probably means the sync
		 * can't continue.
		 */
		fprintf(stderr, _("%s: Can't open \"%s\": %d\n"),
			"GenericConduit::FirstSync",
			_dbinfo->name,
			err);
		add_to_log(_("Error\n"));
		return -1;
	}

	/* Download the database from the Palm to _remotedb */
	_remotedb = pdb_Download(_pconn, _dbinfo, dbh);
	if (_remotedb == 0)
	{
		fprintf(stderr, _("pdb_Download() failed.\n"));
		err = DlpCloseDB(_pconn, dbh);	// Close the database
		add_to_log(_("Error\n"));
		return -1;
	}

	/* Go through each record and clean it up */
	struct pdb_record *remoterec;	// Record in remote database
	struct pdb_record *nextrec;	// Next record on the list

	nextrec = 0;			// No next record (yet)
	for (remoterec = _remotedb->rec_index.rec;
	     remoterec != 0;
	     remoterec = nextrec)
	{
		nextrec = remoterec->next;
					// Remember this, since 'remoterec'
					// might be modified or deleted
					// inside the loop body

		SYNC_TRACE(5)
		{
			fprintf(stderr, "Remote Record:\n");
			fprintf(stderr, "\tID: 0x%08lx\n",
				remoterec->id);
			fprintf(stderr, "\tattributes: 0x%02x ",
				remoterec->attributes);
			if (EXPUNGED(remoterec))
				fprintf(stderr, "EXPUNGED ");
			if (DIRTY(remoterec))
				fprintf(stderr, "DIRTY ");
			if (DELETED(remoterec))
				fprintf(stderr, "DELETED ");
			if (PRIVATE(remoterec))
				fprintf(stderr, "PRIVATE ");
			if (ARCHIVE(remoterec))
				fprintf(stderr, "ARCHIVE ");
			fprintf(stderr, "\n");
		}

		/* This hairy expression just boils down to "if
		 * remoterec is DELETED and ARCHIVED, or at least
		 * DELETED and not EXPUNGED". That's because some
		 * applications aren't polite enough to set the
		 * ARCHIVED flag when they delete a record.
		 */
		if (DELETED(remoterec) &&
		    ((ARCHIVE(remoterec) ||
		     (!EXPUNGED(remoterec)))))
		{
			// Clear flags
			remoterec->attributes &=
				~(PDB_REC_EXPUNGED |
				  PDB_REC_DIRTY |
				  PDB_REC_DELETED |
				  PDB_REC_ARCHIVE);

			// Archive this record
			SYNC_TRACE(5)
				fprintf(stderr, "Archiving this record\n");
			this->archive_record(remoterec);
		} else if (EXPUNGED(remoterec))
		{
			// Delete this record
			SYNC_TRACE(5)
				fprintf(stderr, "Deleting this record\n");
			pdb_DeleteRecordByID(_remotedb,
					     remoterec->id);
		} else {
			SYNC_TRACE(5)
				fprintf(stderr, "Need to save this record\n");
			// Clear flags
			remoterec->attributes &=
				~(PDB_REC_EXPUNGED |
				  PDB_REC_DIRTY |
				  PDB_REC_DELETED |
				  PDB_REC_ARCHIVE);
		}
	}

	// Write the database to its backup file
	err = this->write_backup(_remotedb);
	if (err < 0)
	{
		fprintf(stderr, _("%s error: Can't write backup file.\n"),
			"GenericConduit");
		err = DlpCloseDB(_pconn, dbh);	// Close the database
		add_to_log(_("Error\n"));
		return -1;
	}

	/* Post-sync cleanup */
	if (!IS_RSRC_DB(_remotedb))
	{
		/* Since this function doesn't deal with resource
		 * databases, the test above really just serves to
		 * catch impossible errors.
		 */
		SYNC_TRACE(3)
			fprintf(stderr, "### Cleaning up database.\n");
		err = DlpCleanUpDatabase(_pconn, dbh);
		if (err != DLPSTAT_NOERR)
		{
			fprintf(stderr, _("%s error: can't clean up database: "
				"%d\n"),
				"GenericConduit", err);
			err = DlpCloseDB(_pconn, dbh);
			add_to_log(_("Error\n"));
			return -1;
		}
	}

	SYNC_TRACE(3)
		fprintf(stderr, "### Resetting sync flags.\n");
	err = DlpResetSyncFlags(_pconn, dbh);
	if (err != DLPSTAT_NOERR)
	{
		fprintf(stderr, _("%s error: Can't reset sync flags: %d\n"),
			"GenericConduit", err);
		err = DlpCloseDB(_pconn, dbh);
		add_to_log(_("Error\n"));
		return -1;
	}

	/* Clean up */
	err = DlpCloseDB(_pconn, dbh);	// Close the database

	add_to_log(_("OK\n"));
	return 0; 
}

/* GenericConduit::SlowSync
 * A slow sync is done when the last machine that the Palm synced with
 * was not this one. As a result, we can't trust the records' flags,
 * and must download the entire database and compare it with the local
 * copy.
 */
int
GenericConduit::SlowSync()
{
	int err;
	ubyte dbh;			// Database handle
	struct pdb_record *localrec;	// Record in local database
	struct pdb_record *remoterec;	// Record in remote database

	add_to_log(_dbinfo->name);
	add_to_log(" - ");

	SYNC_TRACE(3)
		fprintf(stderr, "*** Phase 1:\n");
	/* Phase 1: grab the entire database from the Palm */
	err = DlpOpenConduit(_pconn);
	if (err != DLPSTAT_NOERR)
	{
		SYNC_TRACE(4)
			fprintf(stderr, "DlpOpenConduit() returned %d\n",
				err);
		add_to_log(_("Error\n"));
		return -1; 
	}

	/* Open the database for reading */
	err = DlpOpenDB(_pconn,
			CARD0,
			_dbinfo->name,
			DLPCMD_MODE_READ |
			(_dbinfo->db_flags & DLPCMD_DBFLAG_OPEN ?
			 0 :
			 DLPCMD_MODE_WRITE) |
			DLPCMD_MODE_SECRET,
			/* "Secret" records aren't actually secret.
			 * They're actually just the ones marked
			 * private, and they're not at all secret.
			 */
			&dbh);
	switch (err)
	{
	    case DLPSTAT_NOERR:
		/* Things are fine */
		break;
	    case DLPSTAT_NOTFOUND:
	    case DLPSTAT_TOOMANYOPEN:
	    case DLPSTAT_CANTOPEN:
		/* Can't complete this particular operation, but
		 * it's not a show-stopper. The sync can go on.
		 * XXX - Need to indicate to the caller that the sync
		 * can go on.
		 */
		fprintf(stderr, _("%s: Can't open \"%s\": %d\n"),
			"GenericConduit::SlowSync",
			_dbinfo->name, err);
		add_to_log(_("Error\n"));
		return -1;
	    default:
		/* Some other error, which probably means the sync
		 * can't continue.
		 * XXX - Need to indicate this to the caller.
		 */
		fprintf(stderr, _("%s: Can't open \"%s\": %d\n"),
			"GenericConduit::SlowSync",
			_dbinfo->name, err);
		add_to_log(_("Error\n"));
		return -1;
	}

	/* Download the entire remote database */
	_remotedb = pdb_Download(_pconn, _dbinfo, dbh);
	if (_remotedb == 0)
	{
		fprintf(stderr, _("%s error: Can't download \"%s\"\n"),
			"GenericConduit", _dbinfo->name);
		DlpCloseDB(_pconn, dbh);
		add_to_log(_("Error\n"));
		return -1;
	}

	/* XXX - Check the AppInfo block. Since this is a slow sync, we
	 * can't trust the Palm's PDB_ATTR_APPINFODIRTY flag if it's unset.
	 * So assume that the Palm's AppInfo block is dirty and overwrite
	 * the local one.
	 */

	/* Check each remote record in turn, and compare it to the
	 * copy in the local database.
	 */
	SYNC_TRACE(3)
		fprintf(stderr, "Checking remote database entries.\n");
	for (remoterec = _remotedb->rec_index.rec;
	     remoterec != 0;
	     remoterec = remoterec->next)
	{
		SYNC_TRACE(5)
		{
			fprintf(stderr, "Remote Record:\n");
			fprintf(stderr, "\tID: 0x%08lx\n",
				remoterec->id);
			fprintf(stderr, "\tattributes: 0x%02x ",
				remoterec->attributes);
			if (EXPUNGED(remoterec))
				fprintf(stderr, "EXPUNGED ");
			if (DIRTY(remoterec))
				fprintf(stderr, "DIRTY ");
			if (DELETED(remoterec))
				fprintf(stderr, "DELETED ");
			if (PRIVATE(remoterec))
				fprintf(stderr, "PRIVATE ");
			if (ARCHIVE(remoterec))
				fprintf(stderr, "ARCHIVE ");
			fprintf(stderr, "\n");
			debug_dump(stderr, "REM", remoterec->data,
				   remoterec->data_len > 64 ?
					64 : remoterec->data_len);
		}

		/* Look the record up in the local database. */
		localrec = pdb_FindRecordByID(_localdb, remoterec->id);
		if (localrec == 0)
		{
			/* This remote record doesn't exist in the local
			 * database. It has evidently been added since the
			 * last sync with this machine, but it may also
			 * have been deleted since it was added.
			 */

			if (DELETED(remoterec) &&
			    (ARCHIVE(remoterec) ||
			     !EXPUNGED(remoterec)))
			{
				/* This record was deleted. Either it was
				 * explicitly marked as archived, or at
				 * least not explicitly marked as expunged.
				 * So archive it.
				 */
				remoterec->attributes &=
					~(PDB_REC_EXPUNGED |
					  PDB_REC_DIRTY |
					  PDB_REC_DELETED |
					  PDB_REC_ARCHIVE);

				// Archive this record
				SYNC_TRACE(5)
					fprintf(stderr,
						"Archiving this record\n");
				this->archive_record(remoterec);
			} else if (EXPUNGED(remoterec))
			{
				/* This record has been completely deleted */
				SYNC_TRACE(5)
					fprintf(stderr,
						"Deleting this record (local "
						"record doesn't exist, remote "
						"record expunged)\n");
				pdb_DeleteRecordByID(_remotedb, remoterec->id);
			} else {
				struct pdb_record *newrec;

				SYNC_TRACE(5)
					fprintf(stderr,
						"Saving this record\n");
				/* This record is merely new. Clear any
				 * dirty flags it might have, and add it to
				 * the local database.
				 */
				remoterec->attributes &=
					~(PDB_REC_EXPUNGED |
					  PDB_REC_DIRTY |
					  PDB_REC_DELETED |
					  PDB_REC_ARCHIVE);

				// First, make a copy
				newrec = pdb_CopyRecord(_remotedb, remoterec);
				if (newrec == 0)
				{
					fprintf(stderr,
						"Can't copy a new record\n");
					add_to_log(_("Error\n"));
					return -1;
				}

				// Now add the copy to the local database
				pdb_AppendRecord(_localdb, newrec);
			}

			continue;
		}

		/* The remote record exists in the local database. */
		SYNC_TRACE(5)
		{
			fprintf(stderr, "Local Record:\n");
			fprintf(stderr, "\tID: 0x%08lx\n",
				localrec->id);
			fprintf(stderr, "\tattributes: 0x%02x ",
				localrec->attributes);
			if (EXPUNGED(localrec))
				fprintf(stderr, "EXPUNGED ");
			if (DIRTY(localrec))
				fprintf(stderr, "DIRTY ");
			if (DELETED(localrec))
				fprintf(stderr, "DELETED ");
			if (PRIVATE(localrec))
				fprintf(stderr, "PRIVATE ");
			if (ARCHIVE(localrec))
				fprintf(stderr, "ARCHIVE ");
			fprintf(stderr, "\n");
			debug_dump(stderr, "LOC", localrec->data,
				   localrec->data_len > 64 ?
					64 : localrec->data_len);
		}

		/* This remote record exists in the local database. The
		 * main problem here, since there has been a sync that we
		 * don't know about, is that we can't trust the DIRTY flag
		 * on the remote record: if it's set, that means that the
		 * record is dirty, but if it's not set, that doesn't mean
		 * that the record isn't dirty.
		 * We get around this problem by comparing the contents of
		 * the two records, byte by byte. If the contents differ,
		 * then the remote record is dirty (the local one may be
		 * dirty, too, but that's irrelevant.
		 */
		if (!DIRTY(remoterec))
		{
			if (this->compare_rec(localrec, remoterec) != 0)
				/* The records are different. Mark the
				 * remote record as dirty.
				 */
				remoterec->attributes |= PDB_REC_DIRTY;
		}

		/* Sync the two records */
		err = this->SyncRecord(dbh, _localdb, localrec, remoterec);
		SYNC_TRACE(5)
			fprintf(stderr, "SyncRecord returned %d\n ", err);

		/* Mark the remote record as clean for the next phase */
		remoterec->attributes &=
			~(PDB_REC_EXPUNGED |
			  PDB_REC_DIRTY |
			  PDB_REC_DELETED |
			  PDB_REC_ARCHIVE);
	}

	/* Look up each record in the local database and see if it exists
	 * in the remote database. If it does, then we've dealt with it
	 * above. Otherwise, it's a new record in the local database.
	 */
	SYNC_TRACE(3)
		fprintf(stderr, "Checking local database entries.\n");

	struct pdb_record *nextrec;	// Next record in list

	for (localrec = _localdb->rec_index.rec, nextrec = 0;
	     localrec != 0;
	     localrec = nextrec)
	{
		/* 'localrec' might get deleted, so we need to make a note
		 * of the next record in the list now.
		 */
		nextrec = localrec->next;
		/* Try to look this record up in the remote database. */
		remoterec = pdb_FindRecordByID(_remotedb, localrec->id);
		if (remoterec != 0)
		{
			SYNC_TRACE(4)
				fprintf(stderr, "Seen this record already\n");
			/* This local record exists in the remote
			 * database. We've dealt with it already, so
			 * we can skip it now.
			 */
			continue;
		}

		/* This record doesn't exist in the remote database. Deal
		 * with the various possibilities.
		 */

		if (DELETED(localrec) &&
		    (ARCHIVE(localrec) || !EXPUNGED(localrec)))
		{
			/* The local record was deleted, and needs to be
			 * archived.
			 */
			localrec->attributes &=
				~(PDB_REC_EXPUNGED |
				  PDB_REC_DIRTY |
				  PDB_REC_DELETED |
				  PDB_REC_ARCHIVE);

			// Archive this record
			SYNC_TRACE(5)
				fprintf(stderr, "Archiving this record\n");
			this->archive_record(localrec);
		} else if (EXPUNGED(localrec))
		{
			/* The local record has been completely deleted */
			SYNC_TRACE(5)
				fprintf(stderr, "Deleting this record (local "
					"record deleted or something)\n");
			pdb_DeleteRecordByID(_localdb, localrec->id);
		} else if (DIRTY(localrec))
		{
			udword newID;	// ID of uploaded record

			/* This record is merely new. Clear any dirty flags
			 * it might have, and upload it to the Palm.
			 */
			localrec->attributes &=
				~(PDB_REC_EXPUNGED |
				  PDB_REC_DIRTY |
				  PDB_REC_DELETED |
				  PDB_REC_ARCHIVE);

			SYNC_TRACE(6)
				fprintf(stderr, "> Sending local record "
					"(ID 0x%08lx) to Palm\n",
					localrec->id);
			err = DlpWriteRecord(_pconn, dbh, 0x80,
					     localrec->id,
					     /* XXX - The category is the
					      * bottom 4 bits of the
					      * attributes. Fix this
					      * throughout.
					      */
					     localrec->attributes & 0xf0,
					     localrec->attributes & 0x0f,
					     localrec->data_len,
					     localrec->data,
					     &newID);
			if (err != DLPSTAT_NOERR)
			{
				fprintf(stderr, _("Error uploading record "
						  "0x%08lx: %d\n"),
					localrec->id, err);
				return -1;
			}

			/* The record was assigned a (possibly new) unique
			 * ID when it was uploaded. Make sure the local
			 * database reflects this.
			 */
			localrec->id = newID;
			SYNC_TRACE(7)
				fprintf(stderr, "newID == 0x%08lx\n",
					newID);
		} else {
			/* This record is clean but doesn't exist on the
			 * Palm. Archive it, then delete it.
			 * The most likely explanation is that the record
			 * was deleted, after which the user synced with
			 * some other machine, so the record got archived
			 * on that other machine, if it needed to be
			 * archived.
			 * However, the other possibility is that the user
			 * lost his Palm, got a replacement, and is now
			 * syncing with this machine. This is not the Right
			 * Thing to do, but it's a natural mistake, so
			 * archive the record just in case.
			 */
			this->archive_record(localrec);
			pdb_DeleteRecordByID(_localdb, localrec->id);
		}
	}

	/* Write the local database to the backup file */
	err = this->write_backup(_localdb);
	if (err < 0)
	{
		fprintf(stderr, _("%s: Can't write backup file.\n"),
			"GenericConduit::SlowSync");
		err = DlpCloseDB(_pconn, dbh);	// Close the database
		add_to_log(_("Error\n"));
		return -1;
	}

	/* Post-sync cleanup */
	SYNC_TRACE(3)
		fprintf(stderr, "### Cleaning up database.\n");
	err = DlpCleanUpDatabase(_pconn, dbh);
	if (err != DLPSTAT_NOERR)
	{
		fprintf(stderr, _("%s error: Can't clean up database: %d\n"),
			"GenericConduit", err);
		err = DlpCloseDB(_pconn, dbh);
		add_to_log(_("Error\n"));
		return -1;
	}

	SYNC_TRACE(3)
		fprintf(stderr, "### Resetting sync flags.\n");
	err = DlpResetSyncFlags(_pconn, dbh);
	if (err != DLPSTAT_NOERR)
	{
		fprintf(stderr, _("%s error: Can't reset sync flags: %d\n"),
			"GenericConduit", err);
		err = DlpCloseDB(_pconn, dbh);
		add_to_log(_("Error\n"));
		return -1;
	}

	/* Clean up */
	err = DlpCloseDB(_pconn, dbh);	// Close the database

	add_to_log(_("OK\n"));
	return 0;
}

/* FastSync
 * A fast sync is done when the last machine that the Palm synced with
 * is this one. The record flags can be trusted, and we can just check
 * the records that have been marked as modified.
 */
int
GenericConduit::FastSync()
{
	int err;
	struct dlp_recinfo recinfo;	// Next modified record
	const ubyte *rptr;		// Pointer into buffers,
					// for reading
	ubyte dbh;			// Database handle
	struct pdb_record *remoterec;	// Record in remote database
	struct pdb_record *localrec;	// Record in local database

	add_to_log(_dbinfo->name);
	add_to_log(" - ");

	err = DlpOpenConduit(_pconn);
	if (err != DLPSTAT_NOERR)
	{
		SYNC_TRACE(4)
			fprintf(stderr, "GenericConduit::FastSync: "
				"DlpOpenConduit() returned %d\n",
				err);
		add_to_log(_("Error\n"));
		return -1; 
	}

	/* Open the database for reading */
	err = DlpOpenDB(_pconn,
			CARD0,
			_dbinfo->name,
			DLPCMD_MODE_READ |
			(_dbinfo->db_flags & DLPCMD_DBFLAG_OPEN ?
			 0 :
			 DLPCMD_MODE_WRITE) |
			DLPCMD_MODE_SECRET,
			/* "Secret" records aren't actually secret.
			 * They're actually just the ones marked
			 * private, and they're not at all secret.
			 */
			&dbh);
	switch (err)
	{
	    case DLPSTAT_NOERR:
		/* Things are fine */
		break;
	    case DLPSTAT_NOTFOUND:
	    case DLPSTAT_TOOMANYOPEN:
	    case DLPSTAT_CANTOPEN:
		/* Can't complete this particular operation, but it's
		 * not a show-stopper. The sync can go on.
		 */
		fprintf(stderr, _("%s: Can't open \"%s\": %d\n"),
			"GenericConduit::FastSync",
			_dbinfo->name, err);
		add_to_log(_("Error\n"));
		return -1;
	    default:
		/* Some other error, which probably means the sync
		 * can't continue.
		 */
		fprintf(stderr, _("%s: Can't open \"%s\": %d\n"),
			"GenericConduit::FastSync",
			_dbinfo->name, err);
		add_to_log(_("Error\n"));
		return -1;
	}

	/* XXX - Check the AppInfo block. Since this is a fast sync, we can
	 * trust the PDB_ATTR_APPINFODIRTY flag. The four cases are as
	 * follows:
	 *
	 *	Palm	local	Action
	 *	----	-----	------
	 *	clean	clean	Do nothing
	 *	clean	dirty	Upload local AppInfo block to Palm
	 *	dirty	clean	Download Palm's AppInfo block to local copy
	 *	dirty	dirty	Conflict. Palm overwrites desktop (tie-breaker)
	 *			Save AppInfo block to archive file?
	 */

	/* Read each modified record in turn. */
	while ((err = DlpReadNextModifiedRec(_pconn, dbh,
					     &recinfo, &rptr))
	       == DLPSTAT_NOERR)
	{
		SYNC_TRACE(7)
		{
			fprintf(stderr, "Got next modified record:\n");
			fprintf(stderr, "\tsize == %d\n", recinfo.size);
			fprintf(stderr, "\tcategory == %d\n",
				recinfo.category);
			fprintf(stderr, "\tID == 0x%08lx\n", recinfo.id);
			fprintf(stderr, "\tattributes == 0x%04x\n",
				recinfo.attributes);
		}

		/* Got the next modified record. Deal with it */
		remoterec = new_Record(recinfo.attributes,
				       recinfo.category,
				       recinfo.id,
				       recinfo.size,
				       rptr);
		if (remoterec == 0)
		{
			fprintf(stderr,
				_("%s error: Can't allocate new record\n"),
				"GenericConduit");
			this->close_archive();

			DlpCloseDB(_pconn, dbh);
			add_to_log(_("Error\n"));
			return -1;
		}
		SYNC_TRACE(5)
			fprintf(stderr,
				"Created new record from downloaded record\n");

		/* Look up the modified record in the local database
		 * Contract for the first pass: 'localrec', here, is
		 * effectively read-only: it is only a pointer into the
		 * local database. It will be SyncRecord()'s responsibility
		 * to free it if necessary, so this function should not.
		 * However, 'remoterec' is "owned" by this function,
		 * so this function should free it if necessary.
		 * SyncRecord() may not.
		 */
		localrec = pdb_FindRecordByID(_localdb, remoterec->id);
		if (localrec == 0)
		{
			SYNC_TRACE(6)
				fprintf(stderr, "localrec == 0\n");

			/* If the record was created and deleted since the
			 * last sync, it requires special handling.
			 */
			if (DELETED(remoterec))
			{
				if (ARCHIVE(remoterec))
				{
					/* This record was created,
					 * deleted, and marked for
					 * archival, all since the last
					 * sync. Add it to the archive
					 * file.
					 */
					SYNC_TRACE(5)
						fprintf(stderr,
							"This is a new "
							"archived record\n");
					remoterec->attributes &= 0x0f;
						/* XXX - Presumably, this
						 * should just become a
						 * zero assignment, when/if
						 * attributes and
						 * categories get
						 * separated.
						 */

					SYNC_TRACE(5)
						fprintf(stderr,
							"Archiving this record\n");
					this->archive_record(remoterec);
					pdb_FreeRecord(remoterec);
					continue;
				}

				if (EXPUNGED(remoterec))
				{
					/* This record was created,
					 * deleted, and expunged, all since
					 * the last sync. Ignore it.
					 */
					SYNC_TRACE(5)
						fprintf(stderr, "This is a new expunged record\n");
					pdb_FreeRecord(remoterec);
					continue;
				}

				fprintf(stderr,
					"I have a new, deleted record that "
					"is neither archived nor expunged\n"
					"What am I supposed to do?\n");
			}

			/* This record is new. Add it to the local
			 * database.
			 */

			/* Clear the flags in remoterec before adding
			 * it to the local database: it's fresh and
			 * new.
			 */
			remoterec->attributes &= 0x0f;
				/* XXX - Presumably, this should just
				 * become a zero assignment, when/if
				 * attributes and categories get
				 * separated.
				 */

			/* Add the new record to localdb */
			if ((err = pdb_AppendRecord(_localdb, remoterec)) < 0)
			{
				fprintf(stderr,
					_("%s: Can't append new record to "
					  "database: %d\n"),
					"GenericConduit::FastSync",
					err);
				pdb_FreeRecord(remoterec);
				DlpCloseDB(_pconn, dbh);
				add_to_log(_("Error\n"));
				return -1;
			}

			/* Success. Go on to the next modified record */
			continue;
		}

		/* This record already exists in localdb. */
		SYNC_TRACE(5)
			fprintf(stderr, "Found record by ID (0x%08lx)\n",
				remoterec->id);
		err = this->SyncRecord(dbh,
				       _localdb, localrec,
				       remoterec);
		SYNC_TRACE(6)
			fprintf(stderr, "SyncRecord() returned %d\n", err);
		if (err < 0)
		{
			SYNC_TRACE(6)
				fprintf(stderr, "GenericConduit Error: "
					"SyncRecord returned %d\n", err);
			DlpCloseDB(_pconn, dbh);

			this->close_archive();
			return -1;
		}

		pdb_FreeRecord(remoterec);
		// We're done with this record
	}

	/* DlpReadNextModifiedRec() returned an error code. See what
	 * it was and deal accordingly.
	 */
	switch (err)
	{
	    case DLPSTAT_NOTFOUND:
		/* No more modified records found. Skip to the next
		 * part.
		 */
		SYNC_TRACE(5)
			fprintf(stderr,
				"GenericConduit: no more modified records.\n");
		break;
	    default:
		SYNC_TRACE(6)
			fprintf(stderr, "GenericConduit: "
				"DlpReadNextModifiedRec returned %d\n",
				err);
		DlpCloseDB(_pconn, dbh);

		this->close_archive();
		return -1;
	}

	/* Look up each record in the local database. If it's dirty,
	 * deleted, or whatever, deal with it.
	 */
	SYNC_TRACE(3)
		fprintf(stderr, "Checking local database entries.\n");

	struct pdb_record *nextrec;	// Next record in list

	for (localrec = _localdb->rec_index.rec, nextrec = 0;
	     localrec != 0;
	     localrec = nextrec)
	{
		/* 'localrec' might get deleted, so we need to make a note
		 * of the next record in the list now.
		 */
		nextrec = localrec->next;

		/* Deal with the various possibilities. */

		if (DELETED(localrec) &&
		    (ARCHIVE(localrec) || !EXPUNGED(localrec)))
		{
			/* The local record was deleted, and needs to be
			 * archived.
			 */
			localrec->attributes &=
				~(PDB_REC_EXPUNGED |
				  PDB_REC_DIRTY |
				  PDB_REC_DELETED |
				  PDB_REC_ARCHIVE);

			// Archive this record
			SYNC_TRACE(5)
				fprintf(stderr, "Archiving this record\n");
			this->archive_record(localrec);

			err = DlpDeleteRecord(_pconn, dbh, 0, localrec->id);
			switch (err)
			{
			    case DLPSTAT_NOERR:
				// No error
				// * Fall through *

			    case DLPSTAT_NOTFOUND:
				/* No record with this record ID on the
				 * Palm. But that's okay, since we're
				 * deleting it anyway.
				 */

				pdb_DeleteRecordByID(_localdb, localrec->id);
				break;

			    default:
				cerr << "FastSync: Error deleting record 0x"
				     << hex << setw(8) << setfill('0')
				     << ": " << err << endl;
				break;
			}
		} else if (EXPUNGED(localrec))
		{
			/* The local record has been completely deleted */
			SYNC_TRACE(5)
				fprintf(stderr, "Deleting this record\n");

			err = DlpDeleteRecord(_pconn, dbh, 0, localrec->id);
			switch (err)
			{
			    case DLPSTAT_NOERR:
				// No error
				// * Fall through *

			    case DLPSTAT_NOTFOUND:
				/* No record with this record ID on the
				 * Palm. But that's okay, since we're
				 * deleting it anyway.
				 */

				pdb_DeleteRecordByID(_localdb, localrec->id);
				break;

			    default:
				cerr << "FastSync: Error deleting record 0x"
				     << hex << setw(8) << setfill('0')
				     << ": " << err << endl;
				continue;
			}
		} else if (DIRTY(localrec))
		{
			udword newID;	// ID of uploaded record

			/* This record is merely new. Clear any dirty flags
			 * it might have, and upload it to the Palm.
			 */
			localrec->attributes &=
				~(PDB_REC_EXPUNGED |
				  PDB_REC_DIRTY |
				  PDB_REC_DELETED |
				  PDB_REC_ARCHIVE);

			SYNC_TRACE(6)
				fprintf(stderr, "> Sending local record (ID "
					"0x%08lx) to Palm\n",
					localrec->id);
			err = DlpWriteRecord(_pconn, dbh, 0x80,
					     localrec->id,
					     /* XXX - The category is the
					      * bottom 4 bits of the
					      * attributes. Fix this
					      * throughout.
					      */
					     localrec->attributes & 0xf0,
					     localrec->attributes & 0x0f,
					     localrec->data_len,
					     localrec->data,
					     &newID);
			if (err != DLPSTAT_NOERR)
			{
				fprintf(stderr, _("Error uploading record "
						  "0x%08lx: %d\n"),
					localrec->id, err);
				return -1;
			}

			/* The record was assigned a (possibly new) unique
			 * ID when it was uploaded. Make sure the local
			 * database reflects this.
			 */
			localrec->id = newID;
			SYNC_TRACE(7)
				fprintf(stderr, "newID == 0x%08lx\n", newID);
		} else {
			/* This record is clean, but isn't in the list of
			 * records that we downloaded from the Palm. Hence,
			 * its corresponding version on the Palm is also
			 * clean.
			 */
			SYNC_TRACE(5)
				fprintf(stderr, "This record is clean\n");
		}
	}

	/* Write the local database to the backup file */
	err = this->write_backup(_localdb);
	if (err < 0)
	{
		fprintf(stderr, _("%s: Can't write backup file.\n"),
			"GenericConduit::FastSync");
		err = DlpCloseDB(_pconn, dbh);	// Close the database
		add_to_log(_("Error\n"));
		return -1;
	}

	/* Post-sync cleanup */
	if (!DBINFO_ISRSRC(_dbinfo))
	{
		/* Actually, this function doesn't get called for
		 * resource databases, so the test above really checks
		 * a "can't possibly happen" condition.
		 */
		SYNC_TRACE(3)
			fprintf(stderr, "### Cleaning up database.\n");
		err = DlpCleanUpDatabase(_pconn, dbh);
		if (err != DLPSTAT_NOERR)
		{
			fprintf(stderr,
				_("%s error: Can't clean up database: %d\n"),
				"GenericConduit", err);
			err = DlpCloseDB(_pconn, dbh);
			add_to_log(_("Error\n"));
			return -1;
		}
	}

	SYNC_TRACE(3)
		fprintf(stderr, "### Resetting sync flags.\n");
	err = DlpResetSyncFlags(_pconn, dbh);
	if (err != DLPSTAT_NOERR)
	{
		fprintf(stderr, _("%s error: Can't reset sync flags: %d\n"),
			"GenericConduit", err);
		err = DlpCloseDB(_pconn, dbh);
		add_to_log(_("Error\n"));
		return -1;
	}

	/* Clean up */
	err = DlpCloseDB(_pconn, dbh);	// Close the database

	add_to_log(_("OK\n"));
	return 0;		// Success
}

/* GenericConduit::SyncRecord
 * Sync a record in the local database with a remote record.
 *
 * Contract: This function is responsible for freeing 'localrec' if
 * necessary. However, it may not free 'remoterec'.
 */
int
GenericConduit::SyncRecord(
	ubyte dbh,
	struct pdb *localdb,
	struct pdb_record *localrec,
	const struct pdb_record *remoterec)
{
	int err;

	/* Figure out what to do with these records. This is long
	 * and hairy, but the basic form is:
	 *	check each possible case for 'remoterec'
	 *	    check each possible case for 'localrec'
	 *
	 * For each record, there are only four cases to consider:
	 * - DELETED ARCHIVE
	 *	This record has been deleted with archving.
	 * - (DELETED) EXPUNGE
	 *	This record has been deleted without archiving.
	 * - DIRTY
	 *	This record has been modified.
	 * - nothing
	 *	This record hasn't changed.
	 * All other flag combinations are either absurd or redundant. They
	 * should probably be flagged.
	 *
	 * Note: apparently, not all applications are polite enough to set
	 * the DELETED flag when a record is expunged, so need to just
	 * check that flag by itself. However, the ARCHIVE flag overlaps
	 * the category field, so a record that has been deleted with
	 * archiving must have both the DELETED and ARCHIVE flags set.
	 * "AddressBook" sets the DELETE and ARCHIVE flags when you delete
	 * a record and want to archive it.
	 *
	 * For now, I'm going to assume that
	 *	DIRTY & ARCHIVE
	 * and	DELETED & ARCHIVE
	 * both mean "this has been deleted, and should be archived."
	 */
	if ((DELETED(remoterec) || DIRTY(remoterec)) &&
	    ARCHIVE(remoterec))
	{
		SYNC_TRACE(5)
			fprintf(stderr, "Remote: deleted, archived\n");

		/* Remote record has been deleted; user wants an
		 * archive copy.
		 */
		if ((DELETED(localrec) || DIRTY(localrec)) &&
		    ARCHIVE(localrec))
		{
			SYNC_TRACE(5)
				fprintf(stderr, "Local:  deleted, archived\n");

			/* Local record has been deleted; user wants an
			 * archive copy.
			 */

			/* If the contents are identical, archive one copy.
			 * Otherwise, archive both copies.
			 */
			if ((localrec->data_len == remoterec->data_len) &&
			    (memcmp(localrec->data,
				    remoterec->data,
				    localrec->data_len) == 0))
			{
				/* The records are identical */
				this->archive_record(localrec);
			} else {
				/* The records have both been modified, but
				 * in different ways. Archive both of them.
				 */
				this->archive_record(localrec);
				this->archive_record(remoterec);
			}

			/* Delete the record on the Palm */
			SYNC_TRACE(6)
				fprintf(stderr, "> Deleting record on Palm\n");
			err = DlpDeleteRecord(_pconn, dbh, 0,
					      remoterec->id);
			if (err != DLPSTAT_NOERR)
			{
				fprintf(stderr, _("%s: Warning: Can't delete "
						  "record 0x%08lx: %d\n"),
					"SlowSync", remoterec->id, err);
				/* XXX - For now, just ignore this,
				 * since it's probably not a show
				 * stopper.
				 */
			}

			/* Delete localrec */
			SYNC_TRACE(6)
				fprintf(stderr, "> Deleting record in local "
					"database\n");
			pdb_DeleteRecordByID(localdb, localrec->id);

		} else if (EXPUNGED(localrec))
		{
			/* Local record has been deleted without a trace. */
			SYNC_TRACE(5)
				fprintf(stderr, "Local:  deleted, expunged\n");

			/* Archive remoterec */
			SYNC_TRACE(6)
				fprintf(stderr, "> Archiving remote record\n");
			this->archive_record(remoterec);

			/* Delete the record on the Palm */
			SYNC_TRACE(6)
				fprintf(stderr, "> Deleting record on Palm\n");
			err = DlpDeleteRecord(_pconn, dbh, 0,
					      remoterec->id);
			if (err != DLPSTAT_NOERR)
			{
				fprintf(stderr, _("%s: Warning: Can't delete "
						  "record 0x%08lx: %d\n"),
					"SlowSync", remoterec->id, err);
				/* XXX - For now, just ignore this,
				 * since it's probably not a show
				 * stopper.
				 */
			}

			/* Delete localrec */
			SYNC_TRACE(6)
				fprintf(stderr, "> Deleting record in local "
					"database\n");
			pdb_DeleteRecordByID(localdb, localrec->id);

		} else if (DIRTY(localrec))
		{
			udword newID;	/* ID of uploaded record */

			/* Local record has changed */
			SYNC_TRACE(5)
				fprintf(stderr, "Local:  dirty\n");

			/* Archive remoterec */
			SYNC_TRACE(6)
				fprintf(stderr, "> Archiving remote record\n");
			this->archive_record(remoterec);

			/* Fix flags */
			localrec->attributes &= 0x0f;
				/* XXX - This will just be set to 0, once
				 * pdb_record has separate fields for
				 * attributes and category.
				 */
				/* XXX - Actually, don't clear the PRIVATE
				 * flag.
				 */

			/* Upload localrec to Palm */
			SYNC_TRACE(6)
				fprintf(stderr, "> Sending local record (ID "
					"0x%08lx) to Palm\n",
					localrec->id);
			err = DlpWriteRecord(_pconn, dbh, 0x80,
					     localrec->id,
					     /* XXX - The category is the
					      * bottom 4 bits of the
					      * attributes. Fix this
					      * throughout.
					      */
					     localrec->attributes & 0xf0,
					     localrec->attributes & 0x0f,
					     localrec->data_len,
					     localrec->data,
					     &newID);
			if (err != DLPSTAT_NOERR)
			{
				fprintf(stderr, _("Error uploading record "
						  "0x%08lx: %d\n"),
					localrec->id, err);
				return -1;
			}

			/* The record was assigned a (possibly new) unique
			 * ID when it was uploaded. Make sure the local
			 * database reflects this.
			 */
			localrec->id = newID;
			SYNC_TRACE(7)
				fprintf(stderr, "newID == 0x%08lx\n", newID);
		} else {
			/* Local record hasn't changed */
			SYNC_TRACE(5)
				fprintf(stderr, "Local:  clean\n");

			/* Archive remoterec */
			SYNC_TRACE(6)
				fprintf(stderr, "> Archiving remote record\n");
			this->archive_record(localrec);

			/* Delete localrec */
			SYNC_TRACE(6)
				fprintf(stderr, "> Deleting record in local "
					"database\n");
			pdb_DeleteRecordByID(localdb, localrec->id);

		}
	} else if (EXPUNGED(remoterec))
	{
		/* Remote record has been deleted without a trace. */
		SYNC_TRACE(5)
			fprintf(stderr, "Remote: deleted, expunged\n");

		if ((DELETED(localrec) || DIRTY(localrec)) &&
		    ARCHIVE(localrec))
		{
			/* Local record has been deleted; user wants an
			 * archive copy.
			 */
			SYNC_TRACE(5)
				fprintf(stderr, "Local:  deleted, archived\n");

			/* Fix flags */
			localrec->attributes &= 0x0f;

			/* Archive localrec */
			SYNC_TRACE(6)
				fprintf(stderr, "> Archiving local record\n");
			this->archive_record(localrec);

			/* Delete localrec */
			SYNC_TRACE(6)
				fprintf(stderr, "> Deleting record in local "
					"database\n");
			pdb_DeleteRecordByID(localdb, localrec->id);

		} else if (EXPUNGED(localrec))
		{
			/* Local record has been deleted without a trace. */
			SYNC_TRACE(5)
				fprintf(stderr, "Local:  deleted, expunged\n");

			/* Delete localrec */
			SYNC_TRACE(6)
				fprintf(stderr, "> Deleting record in local "
					"database\n");
			pdb_DeleteRecordByID(localdb, localrec->id);

		} else if (DIRTY(localrec))
		{
			/* Local record has changed */

			udword newID;	/* ID of uploaded record */

			SYNC_TRACE(5)
				fprintf(stderr, "Local:  dirty\n");

			/* Delete remoterec */
			SYNC_TRACE(6)
				fprintf(stderr, "> Deleting remote record\n");
			err = DlpDeleteRecord(_pconn, dbh, 0,
					      remoterec->id);
			if (err != DLPSTAT_NOERR)
			{
				fprintf(stderr, _("%s: Warning: Can't delete "
						  "record 0x%08lx: %d\n"),
					"SlowSync", remoterec->id, err);
				/* XXX - For now, just ignore this,
				 * since it's probably not a show
				 * stopper.
				 */
			}

			/* Fix flags */
			localrec->attributes &= 0x0f;
				/* XXX - This will just be 0 once
				 * pdb_record includes separate fields for
				 * attributes and category.
				 */

			/* Upload localrec to Palm */
			SYNC_TRACE(6)
				fprintf(stderr,
					"> Uploading local record to Palm\n");
			err = DlpWriteRecord(_pconn, dbh, 0x80,
					     localrec->id,
					     /* XXX - The bottom nybble of
					      * the attributes byte is the
					      * category. Fix this
					      * throughout.
					      */
					     localrec->attributes & 0xf0,
					     localrec->attributes & 0xf0,
					     localrec->data_len,
					     localrec->data,
					     &newID);
			if (err != DLPSTAT_NOERR)
			{
				fprintf(stderr, _("Error uploading record "
						  "0x%08lx: %d\n"),
					localrec->id, err);
				return -1;
			}

			/* The record was assigned a (possibly new) unique
			 * ID when it was uploaded. Make sure the local
			 * database reflects this.
			 */
			localrec->id = newID;
			SYNC_TRACE(7)
				fprintf(stderr, "newID == 0x%08lx\n", newID);
		} else {
			/* Local record hasn't changed */
			SYNC_TRACE(5)
				fprintf(stderr, "Local:  clean\n");

			/* Delete localrec */
			SYNC_TRACE(6)
				fprintf(stderr, "> Deleting record in local "
					"database\n");
			pdb_DeleteRecordByID(localdb, localrec->id);

		}
	} else if (DIRTY(remoterec))
	{
		/* Remote record has changed */
		SYNC_TRACE(5)
			fprintf(stderr, "Remote: dirty\n");

		/* XXX - Can these next two cases be combined? If the local
		 * record has been deleted, it needs to be deleted. The
		 * only difference is that if it's been archived, need to
		 * archive it first.
		 * XXX - In fact, can this be done throughout?
		 */
		if ((DELETED(localrec) || DIRTY(localrec)) &&
		    ARCHIVE(localrec))
		{
			/* Local record has been deleted; user wants an
			 * archive copy.
			 */
			struct pdb_record *newrec;

			SYNC_TRACE(5)
				fprintf(stderr, "Local:  deleted, archived\n");

			/* Fix flags */
			localrec->attributes &= 0x0f;
				/* XXX - This will just be 0 once
				 * pdb_record includes separate fields for
				 * attributes and category.
				 */

			/* Archive localrec */
			SYNC_TRACE(6)
				fprintf(stderr, "> Archiving local record\n");
			this->archive_record(localrec);
			pdb_DeleteRecordByID(localdb, localrec->id);

			/* Copy remoterec to localdb */
			SYNC_TRACE(6)
				fprintf(stderr, "> Copying remote record to "
					"local database\n");
			newrec = new_Record(remoterec->attributes,
					    remoterec->attributes,
						/* XXX - Category. Sloppy */
					    remoterec->id,
					    remoterec->data_len,
					    remoterec->data);
			if (newrec == 0)
			{
				fprintf(stderr, _("%s: Can't copy record\n"),
					"SyncRecord");
				return -1;
			}

			/* Fix flags */
			localrec->attributes &= 0x0f;
				/* XXX - This will just be 0 once
				 * pdb_record includes separate fields for
				 * attributes and category.
				 */

			pdb_AppendRecord(localdb, newrec);

		} else if (EXPUNGED(localrec))
		{
			/* Local record has been deleted without a trace. */
			struct pdb_record *newrec;

			SYNC_TRACE(5)
				fprintf(stderr, "Local:  deleted, expunged\n");

			/* Delete localrec */
			SYNC_TRACE(6)
				fprintf(stderr, "> Deleting local record\n");
			pdb_DeleteRecordByID(localdb, localrec->id);

			/* Copy remoterec to localdb */
			SYNC_TRACE(6)
				fprintf(stderr, "> Copying remote record to "
					"local database\n");
			newrec = new_Record(remoterec->attributes,
					    remoterec->attributes,
						/* XXX - Category. Sloppy */
					    remoterec->id,
					    remoterec->data_len,
					    remoterec->data);
			if (newrec == 0)
			{
				fprintf(stderr, _("%s: Can't copy record\n"),
					"SyncRecord");
				return -1;
			}

			/* Fix remote flags */
			newrec->attributes &= 0x0f;
				/* XXX - This will just be 0 once
				 * pdb_record includes separate fields for
				 * attributes and category.
				 */

			pdb_AppendRecord(localdb, newrec);

		} else if (DIRTY(localrec))
		{
			/* Local record has changed */
			SYNC_TRACE(5)
				fprintf(stderr, "Local:  dirty\n");

			/* See if the records are identical */
			/* XXX - Use method to compare records */
			if ((localrec->data_len == remoterec->data_len) &&
			    (localrec->data != 0) &&
			    (remoterec->data != 0) &&
			    (memcmp(localrec->data, remoterec->data,
				    localrec->data_len) == 0))
			{
				/* The records are identical.
				 * Reset localrec's flags to clean, but
				 * otherwise do nothing.
				 */
				localrec->attributes &= 0x0f;
					/* XXX - This will just be 0 once
					 * pdb_record includes separate
					 * fields for attributes and
					 * category.
					 */
			} else {
				/* The records have both been modified, but
				 * in different ways.
				 */
				udword newID;	/* ID of uploaded record */
				struct pdb_record *newrec;

				/* Fix flags on localrec */
				localrec->attributes &= 0x0f;
					/* XXX - This will just be 0 once
					 * pdb_record includes separate
					 * fields for attributes and
					 * category.
					 */

				/* Upload localrec to Palm */
				SYNC_TRACE(6)
					fprintf(stderr, "> Uploading local "
						"record to Palm\n");
				err = DlpWriteRecord(_pconn, dbh, 0x80,
						     localrec->id,
						     /* XXX - The bottom
						      * nybble of the
						      * attributes byte is
						      * the category. Fix
						      * this throughout.
						      */
						     localrec->attributes & 0xf0,
						     localrec->attributes & 0xf0,
						     localrec->data_len,
						     localrec->data,
						     &newID);
				if (err != DLPSTAT_NOERR)
				{
					fprintf(stderr, _("Error uploading record 0x%08lx: %d\n"),
						localrec->id, err);
					return -1;
				}

				/* The record was assigned a (possibly new)
				 * unique ID when it was uploaded. Make
				 * sure the local database reflects this.
				 */
				localrec->id = newID;
				SYNC_TRACE(7)
					fprintf(stderr, "newID == 0x%08lx\n",
						newID);

				/* Add remoterec to local database */
				SYNC_TRACE(7)
					fprintf(stderr, "Adding remote record "
						"to local database.\n");
				/* First, make a copy (with clean flags) */
				/* XXX - Get category */
				newrec = new_Record(
					remoterec->attributes &
					~(PDB_REC_EXPUNGED|
					  PDB_REC_DIRTY|
					  PDB_REC_DELETED),
					remoterec->attributes,
						/* XXX - Category. Sloppy */
					remoterec->id,
					remoterec->data_len,
					remoterec->data);
				if (newrec == 0)
				{
					fprintf(stderr, _("Can't copy a new "
							  "record.\n"));
					return -1;
				}

				/* Now add the copy to the local database */
				err = pdb_InsertRecord(localdb, localrec,
						       newrec);
				if (err < 0)
				{
					fprintf(stderr, _("%s: Can't insert "
							  "record 0x%08lx\n"),
						"SyncRecord",
						newrec->id);
					pdb_FreeRecord(newrec);
					return -1;
				}

			}

		} else {
			/* Local record has not changed */
			struct pdb_record *newrec;

			SYNC_TRACE(5)
				fprintf(stderr, "Local:  clean\n");

			SYNC_TRACE(6)
				fprintf(stderr, "> Replacing local record "
					"with remote one\n");
			/* Replace localrec with remoterec.
			 * This is done in three stages:
			 * - copy 'remoterec' to 'newrec'
			 * - insert 'remoterec' after 'localrec'
			 * - delete 'localrec'
			 */
			SYNC_TRACE(6)
				fprintf(stderr, "> Copying remote record to "
					"local database\n");
			newrec = new_Record(remoterec->attributes,
					    remoterec->attributes,
						/* XXX - Category. Sloppy. */
					    remoterec->id,
					    remoterec->data_len,
					    remoterec->data);
			if (newrec == 0)
			{
				fprintf(stderr, _("%s: Can't copy record\n"),
					"SyncRecord");
				return -1;
			}

			/* Fix flags */
			newrec->attributes &= 0x0f;
				/* XXX - This will just be 0 once
				 * pdb_record includes separate fields for
				 * attributes and category.
				 */

			err = pdb_InsertRecord(localdb, localrec, newrec);
			if (err < 0)
			{
				fprintf(stderr, _("%s: Can't insert record 0x%08lx\n"),
					"SlowSync", newrec->id);
				pdb_FreeRecord(newrec);
				return -1;
			}

			pdb_DeleteRecordByID(localdb, localrec->id);
				/* We don't check for errors here, because,
				 * well, we wanted to delete it anyway.
				 */
		}
	} else {
		/* Remote record has not changed */
		SYNC_TRACE(5)
			fprintf(stderr, "Remote: clean\n");

		if ((DELETED(localrec) || DIRTY(localrec)) &&
		    ARCHIVE(localrec))
		{
			/* Local record has been deleted; user wants an
			 * archive copy.
			 */
			SYNC_TRACE(5)
				fprintf(stderr, "Local:  deleted, archived\n");

			/* Fix local flags */
			localrec->attributes &= 0x0f;
				/* XXX - This will just be 0 once
				 * pdb_record includes separate fields for
				 * attributes and category.
				 */

			/* Archive localrec */
			SYNC_TRACE(6)
				fprintf(stderr, "> Archiving local record\n");
			this->archive_record(localrec);

			/* Delete localrec */
			SYNC_TRACE(6)
				fprintf(stderr, "> Deleting record in local "
					"database\n");
			pdb_DeleteRecordByID(localdb, localrec->id);

			err = DlpDeleteRecord(_pconn, dbh, 0,
					      remoterec->id);
			if (err != DLPSTAT_NOERR)
			{
				cerr << "SlowSync: Warning: can't delete record "
				     << hex << setw(8) << setfill('0')
				     << remoterec->id << ": " << err << endl;
				/* XXX - For now, just ignore this,
				 * since it's probably not a show
				 * stopper.
				 */
			}

		} else if (EXPUNGED(localrec))
		{
			/* Local record has been deleted without a trace. */
			SYNC_TRACE(5)
				fprintf(stderr, "Local:  deleted, expunged\n");

			/* Delete localrec */
			SYNC_TRACE(6)
				fprintf(stderr, "> Deleting record in local "
					"database\n");
			pdb_DeleteRecordByID(localdb, localrec->id);

			err = DlpDeleteRecord(_pconn, dbh, 0,
					      remoterec->id);
			if (err != DLPSTAT_NOERR)
			{
				cerr << "SlowSync: Warning: can't delete record "
				     << hex << setw(8) << setfill('0')
				     << remoterec->id << ": " << err << endl;
				/* XXX - For now, just ignore this,
				 * since it's probably not a show
				 * stopper.
				 */
			}

		} else if (DIRTY(localrec))
		{
			/* Local record has changed */
			udword newID;	/* ID of uploaded record */

			SYNC_TRACE(5)
				fprintf(stderr, "Local:  dirty\n");

			/* Fix local flags */
			localrec->attributes &= 0x0f;
				/* XXX - This will just be 0 once
				 * pdb_record includes separate fields for
				 * attributes and category.
				 */

			/* Upload localrec to Palm */
			SYNC_TRACE(6)
				fprintf(stderr, "> Uploading local record to "
					"Palm\n");
			err = DlpWriteRecord(_pconn, dbh, 0x80,
					     localrec->id,
					     /* XXX - The bottom nybble of
					      * the attributes byte is the
					      * category. Fix this
					      * throughout.
					      */
					     localrec->attributes & 0xf0,
					     localrec->attributes & 0xf0,
					     localrec->data_len,
					     localrec->data,
					     &newID);
			if (err != DLPSTAT_NOERR)
			{
				fprintf(stderr, _("Error uploading record "
						  "0x%08lx: %d\n"),
					localrec->id, err);
				return -1;
			}

			/* The record was assigned a (possibly new) unique
			 * ID when it was uploaded. Make sure the local
			 * database reflects this.
			 */
			localrec->id = newID;
			SYNC_TRACE(7)
				fprintf(stderr, "newID == 0x%08lx\n", newID);
		} else {
			/* Local record hasn't changed */
			SYNC_TRACE(5)
				fprintf(stderr, "Local:  clean\n");

			/* Do nothing */
			SYNC_TRACE(6)
				fprintf(stderr, "> Not doing anything\n");
		}
	}

	return 0;	/* Success */
}

/* GenericConduit::compare_rec
 * Compare two records, in the manner of 'strcmp()'. If rec1 is "less than"
 * rec2, return -1. If they are equal, return 0. If rec1 is "greater than"
 * rec2, return 1.
 * XXX - This is a naive and sub-optimal implementation. It remains to be
 * seen whether it's worth optimizing, though.
 */
int
GenericConduit::compare_rec(const struct pdb_record *rec1,
			    const struct pdb_record *rec2)
{
	int i;

	for (i = 0; i < rec1->data_len; ++i)
	{
		if (i >= rec2->data_len)
		{
			/* Got to end of rec2 before the end of rec1, but
			 * they've been equal so far, so rec2 is "smaller".
			 */
			return 1;
		}

		/* Compare the next byte */
		if (rec1->data[i] < rec2->data[i])
			/* rec1 is "less than" rec2 */
			return -1;
		else if (rec1->data[i] > rec2->data[i])
			/* rec1 is "greater than" rec2 */
			return 1;
	}

	/* The two records are equal over the entire length of rec1 */
	if (rec1->data_len < rec2->data_len)
		return 1;

	return 0;	/* Length is the same and data is the same */
}

/* GenericConduit::open_archive
 * A convenience function: records the fact that we're starting on a
 * database that might need to be archived. But don't do anything just yet,
 * since there may not be anything to archive, in which case we don't want
 * an empty archive file lying around.
 */
/* XXX - This approach is flawed, in that we may not find out until the
 * middle of the sync that the archive file can't be opened (e.g.,
 * permissions problem or something). It may be better to open the archive
 * file now and, if it had to be created, remember this fact. Then, if
 * nothing was written to it, delete it at the end.
 */
int
GenericConduit::open_archive()
{
	/* If the archive file was opened, close it.
	 */
	if (_archfd != -1)
		close(_archfd);

	_archfd = -1;

	return 0;
}

/* GenericConduit::archive_record
 * Archive the record 'rec'. If the archive file is not yet open, open it
 * now. If it doesn't exist, create it.
 */
int
GenericConduit::archive_record(const struct pdb_record *rec)
{
	struct arch_record arec;

	/* If no archive file has been opened yet, open one, creating it if
	 * necessary.
	 */
	if (_archfd < 0)
	{
		if ((_archfd = arch_open(_dbinfo, O_WRONLY)) < 0)
		{
			SYNC_TRACE(2)
				fprintf(stderr, "Can't open \"%s\". "
					"Attempting to create\n",
					_dbinfo->name);
			if ((_archfd = arch_create(_dbinfo)) < 0)
			{
				fprintf(stderr, _("Can't create \"%s\"\n"),
					_dbinfo->name);
				return -1;
			}
		}
	}

	/* Write the record */
	arec.type = ARCHREC_REC;
	arec.data_len = rec->data_len;
	arec.data = rec->data;
	return arch_writerecord(_archfd, &arec);
}

/* GenericConduit::close_archive
 * A convenience function: we're done with the archive file. If nothing was
 * archived, do nothing. Otherwise, close the archive file and stuff.
 */
int
GenericConduit::close_archive()
{
	/* If an archive file was opened, close it.
	 */
	if (_archfd != -1)
		close(_archfd);

	_archfd = -1;

	return 0;
}

/* read_backup
 * Read the backup file and put it in _localdb. If the backup file doesn't
 * exist, that's not considered an error: just return 0, but leave _localdb
 * as having value 0 as well.
 * Returns 0 if successful, -1 in case of error.
 */
int
GenericConduit::read_backup()
{
	int err;
	const char *bakfname;		// Backup filename

	bakfname = mkbakfname(_dbinfo);
			/* Construct the full pathname of the backup file */

	/* See if the backup file exists */
	struct stat statbuf;

	err = stat(bakfname, &statbuf);
	if (err < 0)
	{
		SYNC_TRACE(3)
			perror("read_backup: stat");
		if (errno == ENOENT)
		{
			/* The backup file doesn't exist. This isn't
			 * technically an error.
			 */
			/* XXX - Perhaps run the appropriate Fetch conduit
			 * to create the database.
			 */
			_localdb = 0;
			return 0;
		} 

		/* There's a serious problem: it's not just that the file
		 * doesn't exist; stat() failed for some other reason
		 * (e.g., directory permissions). This is serious enough to
		 * abort this conduit altogether.
		 */
		return -1;
	}

	/* Load backup file to _localdb */
	int infd;		// File descriptor for backup file
	if ((infd = open(bakfname, O_RDONLY)) < 0)
	{
		fprintf(stderr, _("%s error: Can't open \"%s\"\n"),
			"read_backup", bakfname);
		return -1;
	}
	_localdb = pdb_Read(infd);
	close(infd);

	if (_localdb == 0)
	{
		fprintf(stderr, _("%s error: Can't load \"%s\"\n"),
			"read_backup", bakfname);
		return -1; 
	}

	return 0;
}

/* write_backup
 * Write 'db' to the backup file. Returns 0 if successful, -1 in case
 * of error.
 * Writing the file is done in two parts: first, write the file to a
 * staging file. Then rename() the staging file to the real backup
 * file. That way, if there's an error halfway through writing the
 * file, the real backup doesn't get clobbered.
 */
int
GenericConduit::write_backup(struct pdb *db)
{
	int err;
	int outfd;			// Output file descriptor
	char bakfname[MAXPATHLEN+1];	// Name of output file
	char stage_fname[MAXPATHLEN+1];	// Name of staging file
	// XXX - ARGH. Really need to convert this XXXXXX into a unique
	// temporary file name. Or maybe just use mkstemp() later on.
	const char* stage_ext = ".XXXXXX";	// Staging file extension
	const int stage_ext_len = strlen(stage_ext);
					// Length of staging file extension

	/* Construct the full pathname of the backup file */
	strncpy(bakfname, mkbakfname(_dbinfo), MAXPATHLEN);
	bakfname[MAXPATHLEN] = '\0';	// Terminate pathname, just in case

	/* Construct the full pathname of the file we'll use for staging
	 * the write.
	 */
	strncpy(stage_fname, bakfname, MAXPATHLEN-stage_ext_len);
	strncat(stage_fname, stage_ext, stage_ext_len);
	if (mktemp(stage_fname) == 0)
	{
		fprintf(stderr, _("%s: Can't create staging file name\n"),
			"GenericConduit::write_backup");
		return -1;
	}

	/* Open the output file */
	if ((outfd = open(stage_fname,
			  O_WRONLY | O_CREAT | O_EXCL,
			  0600)) < 0)
	{
		fprintf(stderr, _("%s: Can't create staging file \"%s\"\n"),
			"GenericConduit::write_backup", stage_fname);
		return -1;
	}
	/* XXX - Lock the file */

	err = pdb_Write(db, outfd);
	if (err < 0)
	{
		fprintf(stderr, _("%s: Can't write staging file \"%s\"\n"),
			"GenericConduit::write_backup", stage_fname);
		close(outfd);
		return err;
	}
	close(outfd);

	/* Rename the staging file */
	err = rename(stage_fname, bakfname);
	if (err < 0)
	{
		fprintf(stderr, _("%s: Can't rename staging file.\n"
				  "Backup left in \"%s\", hopefully\n"),
			"GenericConduit::write_backup",
			stage_fname);
		return err;
	}

	return 0;		// Success
}

/* This is for Emacs's benefit:
 * Local Variables: ***
 * fill-column:	75 ***
 * End: ***
 */
