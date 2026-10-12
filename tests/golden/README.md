# Golden snapshots

`snapshot_*.txt` is the exact text `renderSnapshot()` (`app/src/advisor/snapshot_renderer.cpp`) produces for a
synthetic frame, which is what the model reads as its second system block. The tests compare byte for byte
(each file ends in one newline that is not part of the text).

| File | Frame |
|---|---|
| `snapshot_rich` | every section: 10 of 14 vassals listed, two wars (one as attacker, one as ally), claims, a faction against the ruler and the ruler's own |
| `snapshot_minimal` | `BEGIN` and `END` only: every section "not exported" |
| `snapshot_empty` | every section present but empty, a debt and a negative income, a probe-mode export |
| `snapshot_hostile` | names made to look like instructions and markers; shows what the hygiene leaves |
| `snapshot_none_yet` | the block used while no frame has arrived |

All names are invented ("Test Ruler One"). The frames are built in `app/tests/advisor_test_support.cpp` from
bare `CURIA1` records and parsed by the real parser and assembler.

To rewrite them after an intended change: `CURIA_UPDATE_GOLDEN=1 ./curia_tests -tc="snapshot goldens*"` from the
build directory, then read the diff.
