/**
 * Every turning and first-step value Responsive movement sets, in % of the game's own value (100 as the console),
 * written by hle_trainer.c each frame. (The 2026-11-15 sliders that tuned them were taken out again by request: one
 * Responsive movement switch, these its values.)
 *
 * X(field, label, game variable address, the game's value, kind, Responsive movement's %: tuned in the game by
 * hand, 2026-11-15, and made the standard; the turn on the spot back near the game's, issue #8)
 *   kind: 0 a turn speed in degrees a second (stored radians a ms; the game's while carrying)
 *         1 a time in ms (stored as an integer)
 *         2 a plain number
 *         3 the easing zone in degrees (stored radians; the game's while carrying)
 */
#ifndef HPPOA_TURN_TUNES_H
#define HPPOA_TURN_TUNES_H

#define HPPOA_TURN_TUNES(X)                                                                                \
    X(tune_run_turn, "Run turn speed (RunTurnSpeed)", 0x36AB10u, 350.0f, 0, 200)                         \
    X(tune_walk_turn, "Walk turn speed (WalkTurnSpeed)", 0x36AE34u, 360.0f, 0, 201)                      \
    X(tune_sneak_turn, "Sneak turn speed (SneakTurnSpeed)", 0x36AA7Cu, 90.0f, 0, 199)                    \
    X(tune_shuffle_turn, "Shuffle turn speed (ShuffleTurnSpeed)", 0x36AEC4u, 90.0f, 0, 200)              \
    X(tune_stand_pivot, "Standing pivot (StandPivotSpeed)", 0x36AE9Cu, 360.0f, 0, 204)                   \
    X(tune_sneak_pivot, "Sneaking pivot (SneakPivotSpeed)", 0x36AD64u, 360.0f, 0, 205)                   \
    X(tune_walk_pivot, "Walking pivot (WalkPivotSpeed)", 0x36A964u, 1080.0f, 0, 205)                     \
    X(tune_run_pivot, "Running sharp turn (RunPivotSpeed)", 0x36AC20u, 2160.0f, 0, 207)                  \
    X(tune_shuffle_pivot, "Shuffle pivot (ShufflePivotSpeed)", 0x36AC8Cu, 90.0f, 0, 200)                 \
    X(tune_ease_zone, "Easing zone (TurnSpeedModActive)", 0x36AA10u, 80.0f, 3, 50)                       \
    X(tune_turn_drag, "Speed lost in turns (TurnSpeedDragMult)", 0x36AEFCu, 0.2f, 2, 0)                  \
    X(tune_pivot_wait, "Wait before turning on the spot (JumpTurnPivotDelayTime)", 0x36AD10u, 180.0f, 1, 0) \
    X(tune_shuffle_time, "Shuffle before the first step (ShuffleTime)", 0x36A930u, 100.0f, 1, 0)        \
    X(tune_jt_stand_pivot, "Turn on the spot from standing (JumpTurnStandPivotSpeed)", 0x36AD2Cu, 600.0f, 0, 99) \
    X(tune_jt_run_pivot, "Turnaround at a run (JumpTurnRunPivotSpeed)", 0x36A944u, 600.0f, 0, 204)       \
    X(tune_jt_qturn_pivot, "Skid turn (JumpTurnRunQTurnPivotSpeed)", 0x36AE48u, 675.0f, 0, 208)          \
    X(tune_jt_push_off, "Push off after the turn (JumpTurnPushOffAccel)", 0x36AC50u, 5.0f, 2, 68)       \
    X(tune_jt_qturn_push, "Push off after the skid (JumpTurnRunQTurnPushOffAccel)", 0x36AC7Cu, 2.25f, 2, 210) \
    X(tune_jt_qturn_short, "Short skid push off (JumpTurnRunQTurnShortPushOffAccel)", 0x36AA90u, 1.0f, 2, 209)

#endif
