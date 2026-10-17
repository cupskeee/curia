# Golden snapshots

`snapshot_*.txt` is the exact text `renderSnapshot()` (`app/src/advisor/snapshot_renderer.cpp`) produces for a
synthetic frame, which is what the model reads as its second system block. The tests compare byte for byte
(each file ends in one newline that is not part of the text).

| File | Frame |
|---|---|
| `snapshot_rich` | every section: 10 of 14 vassals listed, two wars (one as attacker, one as ally), claims, a faction against the ruler and the ruler's own |
| `snapshot_minimal` | `BEGIN` and `END` only: every section "not shown" |
| `snapshot_empty` | every section present but empty, a debt and a negative income, a probe-mode export (its header names "a Curia button") |
| `snapshot_hostile` | names made to look like instructions and markers (the old `GAME DATA` ones and the block's own end marker `END OF WHAT THE ADVISOR CAN SEE`, plain, in mixed case, fullwidth, with no-break spaces, with zero-width spaces, and drawn with small equals signs, a rarer format character and a braille blank); shows what the hygiene leaves |
| `snapshot_none_yet` | the block used while no frame has arrived ("Nothing has been read from the game yet") |

All names are invented ("Test Ruler One"). The frames are built in `app/tests/advisor_test_support.cpp` from
bare `CURIA1` records and parsed by the real parser and assembler.

To rewrite them after an intended change: `CURIA_UPDATE_GOLDEN=1 ./curia_tests -tc="snapshot goldens*"` from the
build directory, then read the diff.
