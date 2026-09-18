/* Gap-ledger test harness.  Links the real scrollback.c against a
 * scratch directory; asserts print PASS/FAIL and set the exit code. */
#include "config.h"
#include <stdio.h>
#include <string.h>
#include <glib.h>
#include "scrollback.h"

/* --- app stubs (scrollback.c's only outside deps) --- */
static const char *test_xdir = ".";
char *get_xdir (void) { return (char *) test_xdir; }
void poxchat_timing_log (const char *fmt, ...) { (void) fmt; }
/* scrollback.c strips color before FTS-indexing; the harness's test data is
 * color-free, so a plain copy matches strip_color's semantics here. */
char *strip_color (const char *text, int len, int flags)
{
	(void) flags;
	if (len == -1) len = (int) strlen (text);
	return g_strndup (text, len);
}

static int failures = 0;
#define CHECK(name, cond) do { \
	if (cond) printf ("PASS: %s\n", name); \
	else { printf ("FAIL: %s\n", name); failures++; } \
} while (0)

static scrollback_gap *
nth_gap (GList *l, int n) { return (scrollback_gap *) g_list_nth_data (l, n); }

int
main (int argc, char **argv)
{
	scrollback_db *db;

	if (argc > 1)
		test_xdir = argv[1];	/* scratch dir; harness creates <dir>/scrollback/ */

	db = scrollback_open ("gaptest");
	if (!db) { printf ("FAIL: open\n"); return 1; }

	/* seed messages: 1000..1100 contiguous, hole, 90000..90100 */
	scrollback_begin_transaction (db);
	for (int i = 0; i <= 10; i++)
		scrollback_db_save (db, "#t", 1000 + i * 10, NULL, "early", TRUE);
	for (int i = 0; i <= 10; i++)
		scrollback_db_save (db, "#t", 90000 + i * 10, NULL, "late", TRUE);
	scrollback_commit_transaction (db);

	/* record + list roundtrip */
	gint64 id1 = scrollback_gap_record (db, "#t", 1100, "m-start", 90000, "m-end",
	                                    SCROLLBACK_GAP_WITNESSED);
	CHECK ("record returns id", id1 > 0);
	{
		GList *l = scrollback_gap_list (db, "#t");
		CHECK ("list count 1", g_list_length (l) == 1);
		CHECK ("list bounds", nth_gap (l, 0)->start_ts == 1100 &&
		                      nth_gap (l, 0)->end_ts == 90000);
		CHECK ("list msgids", g_strcmp0 (nth_gap (l, 0)->start_msgid, "m-start") == 0 &&
		                      g_strcmp0 (nth_gap (l, 0)->end_msgid, "m-end") == 0);
		scrollback_gap_list_free (l);
	}

	/* merge on overlap: overlapping candidate folds into the witnessed gap */
	gint64 id2 = scrollback_gap_record (db, "#t", 500, NULL, 2000, NULL,
	                                    SCROLLBACK_GAP_CANDIDATE);
	{
		GList *l = scrollback_gap_list (db, "#t");
		CHECK ("merge count 1", g_list_length (l) == 1);
		CHECK ("merge union", nth_gap (l, 0)->start_ts == 500 &&
		                      nth_gap (l, 0)->end_ts == 90000);
		CHECK ("merge keeps witnessed", nth_gap (l, 0)->state == SCROLLBACK_GAP_WITNESSED);
		CHECK ("merge keeps end msgid", g_strcmp0 (nth_gap (l, 0)->end_msgid, "m-end") == 0);
		id2 = nth_gap (l, 0)->id;
		scrollback_gap_list_free (l);
	}

	/* touch increments and returns; shrink resets */
	CHECK ("touch 1", scrollback_gap_touch (db, id2) == 1);
	CHECK ("touch 2", scrollback_gap_touch (db, id2) == 2);
	scrollback_gap_shrink (db, id2, 0, NULL, 50000, "m-newend");
	{
		scrollback_gap g;
		CHECK ("get after shrink", scrollback_gap_get (db, id2, &g));
		CHECK ("shrink end moved", g.end_ts == 50000 &&
		                           g_strcmp0 (g.end_msgid, "m-newend") == 0);
		CHECK ("shrink kept start", g.start_ts == 500);
		CHECK ("shrink reset attempts", g.attempts == 0 && g.last_attempt == 0);
		scrollback_gap_clear (&g);
	}

	/* state transitions; dead gaps don't merge */
	scrollback_gap_set_state (db, id2, SCROLLBACK_GAP_DEAD);
	{
		gint64 id3 = scrollback_gap_record (db, "#t", 400, NULL, 60000, NULL,
		                                    SCROLLBACK_GAP_WITNESSED);
		GList *l = scrollback_gap_list (db, "#t");
		CHECK ("dead not merged", g_list_length (l) == 2 && id3 > 0 && id3 != id2);
		scrollback_gap_list_free (l);
		scrollback_gap_delete (db, id3);
	}

	/* direction-agnostic shrink guard: a one-sided shrink still works
	 * normally in either direction, but a shrink that would invert the
	 * bounds (new start_ts >= existing end_ts) must delete the row
	 * instead of leaving a nonsensical record behind. */
	{
		gint64 id4 = scrollback_gap_record (db, "#invert", 1000, "s1", 2000, "e1",
		                                    SCROLLBACK_GAP_WITNESSED);
		CHECK ("invert setup", id4 > 0);

		/* normal one-sided shrink: start moves up, end untouched */
		scrollback_gap_shrink (db, id4, 1500, "s2", 0, NULL);
		{
			scrollback_gap g;
			CHECK ("normal start-side shrink", scrollback_gap_get (db, id4, &g) &&
				g.start_ts == 1500 && g.end_ts == 2000 &&
				g_strcmp0 (g.start_msgid, "s2") == 0);
			scrollback_gap_clear (&g);
		}

		/* inverting shrink: new start moves past the existing end -> the
		 * row is deleted rather than left inverted */
		scrollback_gap_shrink (db, id4, 2500, "s3", 0, NULL);
		{
			scrollback_gap g;
			CHECK ("inverting shrink deletes row", !scrollback_gap_get (db, id4, &g));
		}
	}

	/* ordinal: 11 rows sort before ts 90000 */
	CHECK ("ordinal", scrollback_gap_ordinal (db, "#t", 90000) == 11);

	/* bootstrap: candidate over a >12h silence in a fresh channel.
	 * NOTE: do not call bootstrap before seeding — the first call sets
	 * the one-shot latch regardless of row count. */
	{
		scrollback_begin_transaction (db);
		scrollback_db_save (db, "#boot", 1000, "b1", "a", TRUE);
		scrollback_db_save (db, "#boot", 2000, "b2", "b", TRUE);
		scrollback_db_save (db, "#boot", 200000, "b3", "c", TRUE);
		scrollback_commit_transaction (db);
		CHECK ("bootstrap finds hole", scrollback_gap_bootstrap (db, "#boot", 12 * 3600) == 1);
		{
			GList *l = scrollback_gap_list (db, "#boot");
			CHECK ("bootstrap candidate", g_list_length (l) == 1 &&
				nth_gap (l, 0)->state == SCROLLBACK_GAP_CANDIDATE &&
				nth_gap (l, 0)->start_ts == 2000 &&
				nth_gap (l, 0)->end_ts == 200000 &&
				g_strcmp0 (nth_gap (l, 0)->start_msgid, "b2") == 0 &&
				g_strcmp0 (nth_gap (l, 0)->end_msgid, "b3") == 0);
			scrollback_gap_list_free (l);
		}
		CHECK ("bootstrap latched", scrollback_gap_bootstrap (db, "#boot", 12 * 3600) == -1);
	}

	/* bootstrap: a "pending:*" msgid is a client-local echo-message
	 * placeholder the server has never heard of.  The row's timestamp is
	 * real activity and must still bound the gap, but the placeholder
	 * itself must not be snapshotted as the boundary msgid. */
	{
		scrollback_begin_transaction (db);
		scrollback_db_save (db, "#bootpending", 1000, "p1", "a", TRUE);
		scrollback_db_save (db, "#bootpending", 200000, "pending:zzz", "b", TRUE);
		scrollback_commit_transaction (db);
		CHECK ("bootstrap pending finds hole", scrollback_gap_bootstrap (db, "#bootpending", 12 * 3600) == 1);
		{
			GList *l = scrollback_gap_list (db, "#bootpending");
			CHECK ("bootstrap suppresses pending placeholder", g_list_length (l) == 1 &&
				nth_gap (l, 0)->start_ts == 1000 &&
				nth_gap (l, 0)->end_ts == 200000 &&
				g_strcmp0 (nth_gap (l, 0)->start_msgid, "p1") == 0 &&
				nth_gap (l, 0)->end_msgid == NULL);
			scrollback_gap_list_free (l);
		}
	}

	/* delete_candidates: clears bootstrap false-positives (CANDIDATE) for a
	 * channel while preserving WITNESSED (real observations) and DEAD
	 * (settled conclusions).  Models the query-window cleanup: a DM gets
	 * carpet-bombed with candidates by the old unconditional bootstrap. */
	{
		gint64 gc, gw, gd;
		gc = scrollback_gap_record (db, "#cand", 1000, NULL, 100000, NULL,
		                            SCROLLBACK_GAP_CANDIDATE);
		gw = scrollback_gap_record (db, "#cand", 200000, "w1", 300000, "w2",
		                            SCROLLBACK_GAP_WITNESSED);
		gd = scrollback_gap_record (db, "#cand", 400000, NULL, 500000, NULL,
		                            SCROLLBACK_GAP_WITNESSED);
		scrollback_gap_set_state (db, gd, SCROLLBACK_GAP_DEAD);
		CHECK ("delcand setup", gc > 0 && gw > 0 && gd > 0);

		CHECK ("delcand deletes only candidates",
		       scrollback_gap_delete_candidates (db, "#cand") == 1);
		{
			GList *l = scrollback_gap_list (db, "#cand");
			gboolean has_w = FALSE, has_d = FALSE, has_c = FALSE;
			GList *it;
			for (it = l; it; it = it->next)
			{
				scrollback_gap *g = it->data;
				if (g->state == SCROLLBACK_GAP_CANDIDATE) has_c = TRUE;
				else if (g->state == SCROLLBACK_GAP_WITNESSED) has_w = TRUE;
				else if (g->state == SCROLLBACK_GAP_DEAD) has_d = TRUE;
			}
			CHECK ("delcand keeps witnessed and dead",
			       g_list_length (l) == 2 && has_w && has_d && !has_c);
			scrollback_gap_list_free (l);
		}
		/* idempotent: a second sweep deletes nothing */
		CHECK ("delcand idempotent",
		       scrollback_gap_delete_candidates (db, "#cand") == 0);
	}

	/* parking: a gap whose END predates the retention cutoff is parked --
	 * hidden from markers and probes -- but its stored state is untouched,
	 * and widening the cutoff (a store relinked) un-parks it.  A gap that
	 * only STARTS before the cutoff is still partly servable: not parked. */
	{
		gint64 old_c, old_w, straddle, recent;
		scrollback_gap g;
		old_c = scrollback_gap_record (db, "#park1", 1000, NULL, 2000, NULL,
		                               SCROLLBACK_GAP_CANDIDATE);
		old_w = scrollback_gap_record (db, "#park2", 3000, "a", 4000, "b",
		                               SCROLLBACK_GAP_WITNESSED);
		straddle = scrollback_gap_record (db, "#park1", 5000, NULL, 20000, NULL,
		                                  SCROLLBACK_GAP_CANDIDATE);
		recent = scrollback_gap_record (db, "#park2", 30000, NULL, 40000, NULL,
		                                SCROLLBACK_GAP_WITNESSED);
		CHECK ("park setup", old_c > 0 && old_w > 0 && straddle > 0 && recent > 0);

		CHECK ("unknown retention parks nothing",
		       scrollback_gap_get (db, old_c, &g) && !scrollback_gap_is_parked (db, &g));
		scrollback_gap_clear (&g);

		scrollback_set_retention_cutoff (db, 10000);
		CHECK ("cutoff readback", scrollback_get_retention_cutoff (db) == 10000);
		CHECK ("old candidate parked, state untouched",
		       scrollback_gap_get (db, old_c, &g) && scrollback_gap_is_parked (db, &g)
		       && g.state == SCROLLBACK_GAP_CANDIDATE);
		scrollback_gap_clear (&g);
		CHECK ("old witnessed parked, state untouched",
		       scrollback_gap_get (db, old_w, &g) && scrollback_gap_is_parked (db, &g)
		       && g.state == SCROLLBACK_GAP_WITNESSED);
		scrollback_gap_clear (&g);
		CHECK ("straddling gap not parked",
		       scrollback_gap_get (db, straddle, &g) && !scrollback_gap_is_parked (db, &g));
		scrollback_gap_clear (&g);
		CHECK ("recent gap not parked",
		       scrollback_gap_get (db, recent, &g) && !scrollback_gap_is_parked (db, &g));
		scrollback_gap_clear (&g);

		/* the bound widens (relink): parked gaps wake with no bookkeeping */
		scrollback_set_retention_cutoff (db, 1500);
		CHECK ("widened bound wakes the old witnessed gap",
		       scrollback_gap_get (db, old_w, &g) && !scrollback_gap_is_parked (db, &g));
		scrollback_gap_clear (&g);
		CHECK ("widened bound wakes the old candidate",
		       scrollback_gap_get (db, old_c, &g) && !scrollback_gap_is_parked (db, &g));
		scrollback_gap_clear (&g);
		scrollback_set_retention_cutoff (db, 0);
	}

	printf (failures ? "RESULT: %d FAILURES\n" : "RESULT: ALL PASS\n", failures);
	return failures ? 1 : 0;
}
