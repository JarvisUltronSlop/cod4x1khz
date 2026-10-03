"""Actual pinned source bytes, read-only; no scanner/config/census mutation."""
from dataclasses import replace
import hashlib
from pathlib import Path
import sys
import unittest
if '--production' in sys.argv:
    sys.argv.remove('--production')
    sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'scripts'))
from finite_registration_enrollment import REVIEWS, _asm_comments, _asm_table, _validate_source

ROOT = Path(__file__).resolve().parents[3]


class NativeFiniteSource(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.raw = {review.family: (ROOT / review.path).read_bytes() for review in REVIEWS}

    def mutation_refuses(self, family, before, after):
        review = next(review for review in REVIEWS if review.family == family)
        original = self.raw[family]
        self.assertEqual(original.count(before), 1)
        modified = original.replace(before, after, 1)
        # A newly reviewed hash alone cannot bypass the native shape guard.
        newly_hashed = replace(review, sha256=hashlib.sha256(modified).hexdigest())
        with self.assertRaises(ValueError):
            _validate_source(newly_hashed, modified)

    def test_actual_four_pinned_files_and_coordinates(self):
        for review in REVIEWS:
            with self.subTest(family=review.family):
                self.assertEqual(tuple(row[0] for row in _validate_source(review, self.raw[review.family])), review.names)

    def test_actual_null_rows_have_exact_four_word_padding(self):
        for review, label, stride in ((REVIEWS[0], 'fields', 7), (REVIEWS[1], 'ent_fields', 4)):
            text = _asm_comments(self.raw[review.family].decode())
            words = _asm_table(text, label, review.table_line)
            self.assertEqual(len(words), len(review.names) * stride + stride + 4)
            self.assertEqual(words[-review.null_tail_words:], ['0x0'] * review.null_tail_words)
            altered = replace(review, null_tail_words=review.null_tail_words - 1)
            with self.assertRaises(ValueError):
                _validate_source(altered, self.raw[review.family])

    def test_actual_entity_tail_edge_cannot_be_dropped(self):
        self.mutation_refuses('entity', b'\tjmp GScr_AddFieldsForClient\r\n', b'\tjmp GScr_AddFieldsForElse\r\n')

    def test_actual_console_loop_is_distinct_from_named_registration(self):
        raw = self.raw['filters']
        self.assertEqual(raw.count(b'\tcall Cvar_RegisterString\r\n'), 3)
        self.assertEqual(len(_validate_source(REVIEWS[3], raw)), 4)
        self.mutation_refuses('filters', b'\tjnz Con_InitChannels_50\r\n',
                             b'\tjnz Con_InitChannels_60\r\n')
        self.mutation_refuses('filters', b'\tcmp ebx, 0x4\r\n\tjnz Con_InitGameMsgChannels_10',
                             b'\tcmp ebx, 0x5\r\n\tjnz Con_InitGameMsgChannels_10')

    def test_actual_comment_decoys_cannot_satisfy_layout(self):
        self.mutation_refuses('hud', b'\tadd edi, 0x1c\r\n',
                             b'\tadd edi, 0x18 ; add edi, 0x1c\r\n')
        self.mutation_refuses('client', b'#define ENTFIELD_MASK 0xc000',
                             b'#define ENTFIELD_MASK 0xb000 // #define ENTFIELD_MASK 0xc000')

    def test_actual_blank_line_before_call_does_not_fake_old_coordinate(self):
        self.mutation_refuses('entity', b'\tcall Scr_AddClassField\r\n',
                             b'\r\n\tcall Scr_AddClassField\r\n')


if __name__ == '__main__':
    unittest.main()
