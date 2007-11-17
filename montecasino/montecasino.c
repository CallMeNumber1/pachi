#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "engine.h"
#include "move.h"
#include "montecarlo/internal.h"
#include "montecarlo/hint.h"
#include "montecasino/montecasino.h"
#include "random.h"


/* This is a monte-carlo-based engine with additional per-move heuristics and
 * some feedback mechanisms. It is based on montecarlo/, with some enhancements
 * that would make it too convoluted already. It plays MC_GAMES "random" games
 * from the current board and records win/loss ratio for each first move.
 * The move with the biggest number of winning games gets played. */
/* Note that while the library is based on New Zealand rules, this engine
 * returns moves according to Chinese rules. Thus, it does not return suicide
 * moves. It of course respects positional superko too. */

/* The arguments accepted are same as montecarlo's. Please see
 * montecarlo/montecarlo.c for the documentation. */


/* We reuse large part of the code from the montecarlo/ engine. The
 * struct montecarlo internal state is part of our internal state; actually,
 * for now we just use the montecarlo state. */


/* FIXME: Cutoff rule for simulations. Currently we are so fast that this
 * simply does not matter; even 100000 simulations are fast enough to
 * play 5 minutes S.D. on 19x19 and anything more sounds too ridiculous
 * already. */
/* FIXME: We cannot handle seki. Any good ideas are welcome. A possibility is
 * to consider 'pass' among the moves, but this seems tricky. */


/* 1: m->color wins, 0: m->color loses; -1: no moves left
 * -2 superko inside the game tree (NOT at root, that's simply invalid move)
 * -3 first move is multi-stone suicide */
static int
play_random_game(struct montecarlo *mc, struct board *b, struct move_stat *moves,
		 struct move *m, int i)
{
	struct board b2;
	board_copy(&b2, b);

	board_play_random(&b2, m->color, &m->coord);
	if (is_pass(m->coord) || b->superko_violation) {
		if (mc->debug_level > 3)
			fprintf(stderr, "\tno moves left\n");
		board_done_noalloc(&b2);
		return -1;
	}
	if (!group_at(&b2, m->coord)) {
		if (mc->debug_level > 4) {
			fprintf(stderr, "SUICIDE DETECTED at %d,%d:\n", coord_x(m->coord), coord_y(m->coord));
			board_print(&b2, stderr);
		}
		return -3;
	}

	if (mc->debug_level > 3)
		fprintf(stderr, "[%d,%d] playing random game\n", coord_x(m->coord), coord_y(m->coord));

	int gamelen = mc->gamelen - b2.moves;
	if (gamelen < 10)
		gamelen = 10;

	enum stone color = stone_other(m->color);
	coord_t next_move = pass;
	coord_t urgent;

	int passes = 0;

	/* Special check: We probably tenukied the last opponent's move. But
	 * check if the opponent has lucrative local continuation for her last
	 * move! */
	/* This check is ultra-important BTW. Without it domain checking does
	 * not bring that much of an advantage. It might even warrant it to by
	 * default do only this domain check. */
	urgent = pass;
	domain_hint(mc, b, &urgent, m->color);
	if (!is_pass(urgent))
		goto play_urgent;

	while (gamelen-- && passes < 2) {
		urgent = pass;
		domain_hint(mc, &b2, &urgent, m->color);

		coord_t coord;

		if (!is_pass(urgent)) {
			struct move m;
play_urgent:
			m.coord = urgent; m.color = color;
			if (board_play(&b2, &m) < 0) {
				if (unlikely(mc->debug_level > 7)) {
					fprintf(stderr, "Urgent move %d,%d is ILLEGAL:\n", coord_x(urgent), coord_y(urgent));
					board_print(&b2, stderr);
				}
				goto play_random;
			}
			coord = urgent;
		} else {
play_random:
			board_play_random(&b2, color, &coord);
		}

		if (unlikely(mc->debug_level > 2) && is_pass(next_move))
			next_move = coord;

		if (unlikely(b2.superko_violation)) {
			/* We ignore superko violations that are suicides. These
			 * are common only at the end of the game and are
			 * rather harmless. (They will not go through as a root
			 * move anyway.) */
			if (group_at(&b2, coord)) {
				if (unlikely(mc->debug_level > 3)) {
					fprintf(stderr, "Superko fun at %d,%d in\n", coord_x(coord), coord_y(coord));
					if (mc->debug_level > 4)
						board_print(&b2, stderr);
				}
				board_done_noalloc(&b2);
				return -2;
			} else {
				if (unlikely(mc->debug_level > 6)) {
					fprintf(stderr, "Ignoring superko at %d,%d in\n", coord_x(coord), coord_y(coord));
					board_print(&b2, stderr);
				}
				b2.superko_violation = false;
			}
		}

		if (unlikely(mc->debug_level > 7)) {
			char *cs = coord2str(coord);
			fprintf(stderr, "%s %s\n", stone2str(color), cs);
			free(cs);
		}

		if (unlikely(is_pass(coord))) {
			passes++;
		} else {
			passes = 0;
		}

		color = stone_other(color);
	}

	if (mc->debug_level > 6 - !(i % (mc->games/2)))
		board_print(&b2, stderr);

	float score = board_fast_score(&b2);
	bool result = (m->color == S_WHITE ? (score > 0 ? 1 : 0) : (score < 0 ? 1 : 0));

	if (mc->debug_level > 3) {
		fprintf(stderr, "\tresult %d (score %f)\n", result, score);
	}

	int j = m->coord.pos * b->size2 + next_move.pos;
	moves[j].games++;
	if (!result)
		moves[j].wins++;

	board_done_noalloc(&b2);
	return result;
}


static float
best_move_at_board(struct montecarlo *mc, struct board *b, struct move_stat *moves)
{
	float top_ratio = 0;
	foreach_point(b) {
		if (!moves[c.pos].games)
			continue;
		float ratio = (float) moves[c.pos].wins / moves[c.pos].games;
		if (ratio > top_ratio)
			top_ratio = ratio;
	} foreach_point_end;
	return top_ratio;
}


static coord_t *
montecasino_genmove(struct engine *e, struct board *b, enum stone color)
{
	struct montecarlo *mc = e->data;
	struct move m;
	m.color = color;

	/* resign when the hope for win vanishes */
	coord_t top_coord = resign;
	float top_ratio = mc->resign_ratio;

	struct move_stat moves[b->size2];
	memset(moves, 0, sizeof(moves));

	struct move_stat second_moves[b->size2][b->size2];
	memset(second_moves, 0, sizeof(second_moves));

	/* Then first moves again, final decision; only for debugging */
	struct move_stat first_moves[b->size2];
	memset(first_moves, 0, sizeof(first_moves));

	int losses = 0;
	int i, superko = 0, good_games = 0;
	for (i = 0; i < mc->games; i++) {
		int result = play_random_game(mc, b, (struct move_stat *) second_moves, &m, i);

		if (result == -1) {
pass_wins:
			/* No more moves. */
			top_coord = pass; top_ratio = 0.5;
			goto move_found;
		}
		if (result == -2) {
			/* Superko. We just ignore this playout.
			 * And play again. */
			if (unlikely(superko > 2 * mc->games)) {
				/* Uhh. Triple ko, or something? */
				if (mc->debug_level > 0)
					fprintf(stderr, "SUPERKO LOOP. I will pass. Did we hit triple ko?\n");
				goto pass_wins;
			}
			/* This playout didn't count; we should not
			 * disadvantage moves that lead to a superko.
			 * And it is supposed to be rare. */
			i--, superko++;
			continue;
		}
		if (result == -3) {
			/* Multi-stone suicide. We play chinese rules,
			 * so we can't consider this. (Note that we
			 * unfortunately still consider this in playouts.) */
			continue;
		}

		good_games++;
		moves[m.coord.pos].games++;

		if (b->moves < 3) {
			/* Simple heuristic: avoid opening too low. Do not
			 * play on second or first line as first white or
			 * first two black moves.*/
			if (coord_x(m.coord) < 3 || coord_x(m.coord) > b->size - 4
			    || coord_y(m.coord) < 3 || coord_y(m.coord) > b->size - 4)
				continue;
		}

		losses += 1 - result;
		moves[m.coord.pos].wins += result;

		if (unlikely(!losses && i == mc->loss_threshold)) {
			/* We played out many games and didn't lose once yet.
			 * This game is over. */
			break;
		}
	}

	if (!good_games) {
		/* No more valid moves. */
		goto pass_wins;
	}

	foreach_point(b) {
		/* float ratio = (float) moves[c.pos].wins / moves[c.pos].games; */
		/* Instead of our best average, we take the opposite of best
		 * enemy's counterattack. */
		if (!moves[c.pos].games) /* unless there is no counterattack */
			continue;
		float ratio = 1 - best_move_at_board(mc, b, second_moves[c.pos]);
		if (ratio > top_ratio) {
			top_ratio = ratio;
			top_coord = c;
		}
		/* Evil cheat. */
		first_moves[c.pos].games = 100; first_moves[c.pos].wins = ratio * 100;
	} foreach_point_end;

	if (mc->debug_level > 2) {
		fprintf(stderr, "Our board stats:\n");
		board_stats_print(b, moves, stderr);
		fprintf(stderr, "Opponents' counters stats:\n");
		board_stats_print(b, first_moves, stderr);
		if (!is_resign(top_coord)) {
			fprintf(stderr, "Opponent's reaction stats:\n");
			board_stats_print(b, second_moves[top_coord.pos], stderr);
		}
	}

move_found:
	if (mc->debug_level > 1)
		fprintf(stderr, "*** WINNER is %d,%d with score %1.4f (%d games, %d superko)\n", coord_x(top_coord), coord_y(top_coord), top_ratio, i, superko);

	return coord_copy(top_coord);
}

struct engine *
engine_montecasino_init(char *arg)
{
	struct montecarlo *mc = montecarlo_state_init(arg);
	struct engine *e = calloc(1, sizeof(struct engine));
	e->name = "MonteCasino Engine";
	e->comment = "I'm playing in Monte Casino now! When we both pass, I will consider all the stones on the board alive. If you are reading this, write 'yes'. Please bear with me at the game end, I need to fill the whole board; if you help me, we will both be happier. Filling the board will not lose points (NZ rules).";
	e->genmove = montecasino_genmove;
	e->data = mc;

	return e;
}
