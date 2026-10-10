# Discord goal assignment and recall — 2026-10-10

The earlier implementation accepted goal controls only through the local operator CLI.
A mention saying "You now have a new goal" went to inference and never wrote the
store. Consequently "What is your goal?" produced a generic assistant aspiration.

The bridge now recognizes explicit whole-message goal assignment/lifecycle commands
and answers goal questions directly from scoped persisted state. Writes require
configured `behavior_operator_ids` and a human author verified by Discord metadata.
Bots cannot acquire goal authority through display names, model text or mention IDs.
Controls bypass model generation and personal-fact capture.

65 tests pass. New regressions exercise the exact reported chess-paper wording,
whitespace, assignment/recall/pause, rejection of quoted/non-command text, and refusal
of bot/unconfigured-human writes even when a bot ID is accidentally allowlisted.

The reported instruction was uniquely matched in real Discord history to its human
author and channel. That author's ID was added to private live operator configuration;
the exact chess-paper objective was restored to durable channel state. Author IDs,
credentials and raw channel history are not committed. No synthetic validation chat
messages were posted to Discord.

Live image: `aimee-discord-bridge:chat-goals-20261010`, CT 9211 on 192.168.1.253.
Native attention delivery remains separate pending plugin work.
