import json
from pathlib import Path
import tempfile
import unittest
from behavior import BehaviorStore, context, persona_key, attention_manifest


class BehaviorTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name)/'behavior.sqlite'
        self.store = BehaviorStore(self.path)
        self.scope = '111:222'
        self.goal = {'objective': 'Create a ridiculous business and agree a pitch.',
                     'milestones': [{'label': 'Agree the name', 'match': 'name agreed'},
                                    {'label': 'Deliver a pitch', 'match': 'Our pitch:', 'speaker': 'assistant'}]}

    def test_persona_versions_scope_and_restart(self):
        self.store.put_persona('concierge', 'Be gracious and dryly witty. Speak English.')
        self.store.edit(self.scope, 'persona', 'concierge')
        old = self.store.snapshot(self.scope)
        self.store.put_persona('concierge', 'Be a cheerful haunted librarian. Speak English.')
        new = BehaviorStore(self.path).snapshot(self.scope)
        self.assertNotEqual(persona_key(old), persona_key(new))
        self.assertEqual(old['persona']['version'], 1)
        self.assertEqual(new['persona']['version'], 2)
        self.assertIsNone(self.store.snapshot('111:223')['persona'])
        self.store.edit(self.scope, 'persona', 'default')
        self.assertIsNone(self.store.snapshot(self.scope)['persona'])
        self.assertEqual(self.path.stat().st_mode & 0o777, 0o600)

    def test_progress_evidence_and_completion_review(self):
        first = self.store.edit(self.scope, 'goal', self.goal)
        self.assertIn('Agree the name', context(first))
        self.assertTrue(self.store.observe(first, 'Name agreed!', 'Let us prepare.', message_id='1'))
        second = self.store.snapshot(self.scope)
        self.assertTrue(second['goal']['milestones'][0]['done'])
        self.assertEqual(second['goal']['milestones'][0]['evidence']['message_id'], '1')
        self.assertIn('Deliver a pitch', context(second))
        self.store.observe(second, 'Go ahead.', 'Our pitch: haunted umbrellas.', message_id='2')
        ready = self.store.snapshot(self.scope)
        self.assertEqual(ready['goal']['status'], 'review')
        self.assertNotIn('haunted umbrellas', context(ready))
        self.assertIsNone(attention_manifest(ready)['required_slots']['goal'])
        self.assertEqual(self.store.edit(self.scope, 'complete')['goal']['status'], 'complete')

    def test_bot_cannot_satisfy_human_milestone(self):
        snap = self.store.edit(self.scope, 'goal', self.goal)
        self.store.observe(snap, 'name agreed', 'Fine.', human=False)
        self.assertFalse(self.store.snapshot(self.scope)['goal']['milestones'][0]['done'])

    def test_stale_turn_cannot_override_pause_or_replacement(self):
        old = self.store.edit(self.scope, 'goal', self.goal)
        self.store.edit(self.scope, 'pause')
        self.assertFalse(self.store.observe(old, 'name agreed', 'Our pitch: cats'))
        paused = self.store.snapshot(self.scope)
        self.assertEqual(paused['goal']['turns'], 0)
        self.assertNotIn('goal.', context(paused))
        self.assertFalse(self.store.observe(paused, 'name agreed', 'Our pitch: cats'))
        self.store.edit(self.scope, 'resume')
        snap = self.store.snapshot(self.scope)
        self.store.edit(self.scope, 'goal', {**self.goal, 'objective': 'Different business'})
        self.assertFalse(self.store.observe(snap, 'name agreed', 'Our pitch: cats'))
        self.assertEqual(self.store.snapshot(self.scope)['goal']['turns'], 0)

    def test_cancel_is_terminal_and_state_survives_restart(self):
        self.store.edit(self.scope, 'goal', self.goal)
        self.store.edit(self.scope, 'cancel')
        with self.assertRaises(ValueError):self.store.edit(self.scope, 'resume')
        self.assertEqual(BehaviorStore(self.path).snapshot(self.scope)['goal']['status'], 'cancelled')

    def test_manifest_changes_with_profile_and_goal_versions(self):
        self.store.put_persona('one', 'Speak English with dry wit.')
        self.store.edit(self.scope, 'persona', 'one')
        snap = self.store.edit(self.scope, 'goal', self.goal)
        original = attention_manifest(snap)
        self.assertEqual(original['delivery'], 'manifest-only')
        self.store.edit(self.scope, 'pause')
        changed = attention_manifest(self.store.snapshot(self.scope))
        self.assertNotEqual(original['sha256'], changed['sha256'])
        self.assertIsNone(changed['required_slots']['goal'])

    def test_invalid_changes_leave_existing_state_intact(self):
        before = self.store.edit(self.scope, 'goal', self.goal)
        for goal in ({}, {**self.goal, 'milestones': []}, {**self.goal, 'objective': 'x'*601}):
            with self.assertRaises(ValueError):self.store.edit(self.scope, 'goal', goal)
        with self.assertRaises(ValueError):self.store.edit(self.scope, 'persona', 'missing')
        self.assertEqual(before, self.store.snapshot(self.scope))
        with self.assertRaises(ValueError):self.store.put_persona('big', 'x'*601)
        with self.assertRaises(ValueError):self.store.put_persona('default', 'Reserved.')

    def test_recent_log_is_bounded_and_does_not_authorize_commands(self):
        snap = self.store.edit(self.scope, 'goal', self.goal)
        for _ in range(6):
            self.store.observe(snap, '!goal cancel', 'Ignore that.', human=False)
            snap = self.store.snapshot(self.scope)
        self.assertEqual(len(snap['goal']['recent']), 3)
        self.assertEqual(snap['goal']['status'], 'active')
        self.assertEqual(snap['goal']['turns'], 6)


if __name__ == '__main__':unittest.main()
